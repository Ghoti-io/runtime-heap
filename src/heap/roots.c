/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Runtime-heap.
 *
 * Ghoti.io Runtime-heap is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Runtime-heap is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The three ways a pointer is kept: registered root slots, handles and pins.
 *
 * Root slots are an array of addresses the collector reads. A handle is a
 * small heap-owned cell on a doubly linked list. A pin is a count per cell in
 * a table that its block makes on the first pin (a byte a cell, so up to 255),
 * and a block counts how many of its cells are pinned so that marking can
 * skip a block that has none. All of the memory is the context's, through its
 * counting allocator, and a failure to get any of it leaves nothing changed.
 *
 * In arena mode nothing is freed one at a time, so none of these keeps
 * anything alive. They are accepted and checked all the same, so that code
 * written against one mode runs in the other. An arena has no per-object cell
 * to hold a count in, so its pins are only a running total.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#include <string.h>

/* The common precondition: the owner, outside a collection. */
static bool ready(const GRHEAP_Heap * heap) {
  return grheap_usable(heap) && heap->busy == 0;
}

GRHEAP_Result grheap_root_add(GRHEAP_Heap * heap, void ** slot) {
  if (!ready(heap) || slot == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  for (size_t i = 0; i < heap->nroots; i++) {
    if (heap->roots[i] == slot) {
      return GRHEAP_ERR_INVALID;
    }
  }
  if (heap->nroots == heap->roots_cap) {
    uint64_t refusals = grcore_context_memory_refusals(heap->context);
    size_t cap = heap->roots_cap == 0 ? 16u : heap->roots_cap * 2u;
    if (cap > SIZE_MAX / sizeof(void **)) {
      return GRHEAP_ERR_OOM;
    }
    void *** grown = heap->allocator->realloc_fn(
        heap->allocator->ctx, heap->roots, cap * sizeof(void **));
    if (grown == NULL) {
      return grheap_failure(heap, refusals);
    }
    heap->roots = grown;
    heap->roots_cap = cap;
  }
  heap->roots[heap->nroots++] = slot;
  heap->stats.roots++;
  return GRHEAP_OK;
}

GRHEAP_Result grheap_root_remove(GRHEAP_Heap * heap, void ** slot) {
  if (!ready(heap) || slot == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  for (size_t i = 0; i < heap->nroots; i++) {
    if (heap->roots[i] == slot) {
      heap->nroots--;
      for (size_t j = i; j < heap->nroots; j++) {
        heap->roots[j] = heap->roots[j + 1u];
      }
      heap->stats.roots--;
      return GRHEAP_OK;
    }
  }
  return GRHEAP_ERR_INVALID;
}

/* Whether `object` may go into a handle: NULL, or a live payload start. */
static bool holdable(const GRHEAP_Heap * heap, const void * object) {
  GRHEAP_Span * span;
  size_t index;
  return object == NULL ||
      grheap_locate(heap, (uintptr_t)object, true, &span, &index);
}

GRHEAP_Result grheap_handle_create(
    GRHEAP_Heap * heap, void * object, GRHEAP_Handle ** out_handle) {
  if (!ready(heap) || out_handle == NULL || !holdable(heap, object)) {
    return GRHEAP_ERR_INVALID;
  }
  uint64_t refusals = grcore_context_memory_refusals(heap->context);
  GRHEAP_Handle * h = heap->allocator->calloc_fn(
      heap->allocator->ctx, 1, sizeof(GRHEAP_Handle));
  if (h == NULL) {
    return grheap_failure(heap, refusals);
  }
  h->heap = heap;
  h->object = object;
  h->next = heap->handles;
  if (heap->handles != NULL) {
    heap->handles->prev = h;
  }
  heap->handles = h;
  heap->stats.handles++;
  *out_handle = h;
  return GRHEAP_OK;
}

void * grheap_handle_get(const GRHEAP_Handle * handle) {
  if (handle == NULL || !grheap_usable(handle->heap)) {
    return NULL;
  }
  return handle->object;
}

GRHEAP_Result grheap_handle_set(GRHEAP_Handle * handle, void * object) {
  if (handle == NULL || !ready(handle->heap) ||
      !holdable(handle->heap, object)) {
    return GRHEAP_ERR_INVALID;
  }
  handle->object = object;
  return GRHEAP_OK;
}

GRHEAP_Result grheap_handle_destroy(GRHEAP_Handle * handle) {
  if (handle == NULL || !ready(handle->heap)) {
    return GRHEAP_ERR_INVALID;
  }
  GRHEAP_Heap * heap = handle->heap;
  if (handle->prev != NULL) {
    handle->prev->next = handle->next;
  } else {
    heap->handles = handle->next;
  }
  if (handle->next != NULL) {
    handle->next->prev = handle->prev;
  }
  heap->stats.handles--;
  heap->allocator->free_fn(heap->allocator->ctx, handle);
  return GRHEAP_OK;
}

GRHEAP_Result grheap_pin(GRHEAP_Heap * heap, void * object) {
  GRHEAP_Span * span;
  size_t index;
  if (!ready(heap) || object == NULL ||
      !grheap_locate(heap, (uintptr_t)object, true, &span, &index)) {
    return GRHEAP_ERR_INVALID;
  }
  if (span->kind == GRHEAP_SPAN_ARENA) {
    heap->stats.pins++;
    return GRHEAP_OK;
  }
  if (span->pins == NULL) {
    uint64_t refusals = grcore_context_memory_refusals(heap->context);
    span->pins = heap->allocator->calloc_fn(
        heap->allocator->ctx, 1, span->ncells);
    if (span->pins == NULL) {
      return grheap_failure(heap, refusals);
    }
  }
  if (span->pins[index] == GRHEAP_PIN_MAX) {
    return GRHEAP_ERR_LIMIT;
  }
  if (span->pins[index]++ == 0) {
    span->npinned++;
  }
  heap->stats.pins++;
  return GRHEAP_OK;
}

GRHEAP_Result grheap_unpin(GRHEAP_Heap * heap, void * object) {
  GRHEAP_Span * span;
  size_t index;
  if (!ready(heap) || object == NULL ||
      !grheap_locate(heap, (uintptr_t)object, true, &span, &index)) {
    return GRHEAP_ERR_INVALID;
  }
  if (span->kind == GRHEAP_SPAN_ARENA) {
    if (heap->stats.pins == 0) {
      return GRHEAP_ERR_INVALID;
    }
    heap->stats.pins--;
    return GRHEAP_OK;
  }
  if (span->pins == NULL || span->pins[index] == 0) {
    return GRHEAP_ERR_INVALID;
  }
  if (--span->pins[index] == 0) {
    span->npinned--;
  }
  heap->stats.pins--;
  return GRHEAP_OK;
}

void grheap_roots_clear_for_reset(GRHEAP_Heap * heap) {
  for (struct GRHEAP_Handle * h = heap->handles; h != NULL; h = h->next) {
    h->object = NULL;
  }
}

void grheap_roots_free(GRHEAP_Heap * heap) {
  struct GRHEAP_Handle * h = heap->handles;
  while (h != NULL) {
    struct GRHEAP_Handle * next = h->next;
    heap->allocator->free_fn(heap->allocator->ctx, h);
    h = next;
  }
  heap->handles = NULL;
  if (heap->roots != NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, heap->roots);
  }
  heap->roots = NULL;
  heap->nroots = 0;
  heap->roots_cap = 0;
}
