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
 * The heap's attachment to a context: its key, its creation, its queries, its
 * poll handler and its teardown.
 *
 * One heap per context, under a cardinality-one key in the ACT phase (AD-19).
 * The key's destructor is the only way a heap dies (AD-20): it runs every
 * object's `destroy` hook, hands every page back to the provider and frees the
 * metadata, in the context's own teardown order. After a context is destroyed
 * the test allocator holds no live block and the context's meter reads zero.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

/* The ACT handler. A poll over the memory budget asks the handlers that can
 * reclaim to do so before it votes (CAP-2, AD-21), and a collection is the
 * reclaim a heap has. An arena has nothing to give back until it is reset. */
static void heap_poll(
    GRCORE_Context * context, void * value, GRCORE_PollCall * call) {
  (void)context;
  GRHEAP_Heap * heap = value;
  if (heap->mode != GRHEAP_MODE_COLLECTED || heap->busy != 0) {
    return;
  }
  /* Torture collects at every poll, and a reclaim request is one more reason
   * to: one collection answers both. */
  bool torture = heap->torture;
  if (torture || grcore_pollcall_reclaim_requested(call)) {
    if (torture) {
      heap->stats.torture_collections++;
    }
    grheap_collect_now(heap);
  }
}

static void heap_key_destroy(GRCORE_Context * context, void * value) {
  (void)context;
  GRHEAP_Heap * heap = value;
  grheap_pending_free(heap);
  grheap_run_all_destroys(heap);
  grheap_release_all_spans(heap);
  if (heap->spans != NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, heap->spans);
  }
  grheap_roots_free(heap);
  grheap_mark_stack_free(heap);
  grheap_shadow_free(heap);
  grheap_weak_free(heap);
  grheap_ids_free(heap);
  heap->allocator->free_fn(heap->allocator->ctx, heap);
}

const GRCORE_Key grheap_key = {"runtime-heap", GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_ACT, heap_key_destroy, heap_poll, grheap_key_snapshot,
    grheap_key_restore, grheap_key_settle};

GRHEAP_Result grheap_heap_create(GRCORE_Context * context,
    const GRHEAP_Options * options, GRHEAP_Heap ** out_heap) {
  if (context == NULL || out_heap == NULL ||
      !grcore_context_is_owner(context) ||
      grcore_context_slot(context, &grheap_key) != NULL) {
    return GRHEAP_ERR_INVALID;
  }
  const GRCORE_Allocator * allocator = grcore_context_allocator(context);
  const GRCORE_PageProvider * pages = grcore_context_page_provider(context);
  if (allocator == NULL || pages == NULL || pages->page_size == 0) {
    return GRHEAP_ERR_INTERNAL;
  }

  uint64_t refusals = grcore_context_memory_refusals(context);
  GRHEAP_Heap * heap = allocator->calloc_fn(allocator->ctx, 1, sizeof *heap);
  if (heap == NULL) {
    return grcore_context_memory_refusals(context) > refusals
        ? GRHEAP_ERR_LIMIT
        : GRHEAP_ERR_OOM;
  }
  heap->context = context;
  heap->allocator = allocator;
  heap->pages = pages;
  heap->block_size = pages->page_size > GRHEAP_BLOCK_MIN ? pages->page_size
                                                         : GRHEAP_BLOCK_MIN;
  if (options != NULL) {
    heap->mode = options->mode;
    heap->codec = options->codec;
    heap->threshold = options->threshold;
    heap->auto_collect = options->auto_collect;
    heap->torture = options->torture;
    heap->verify = options->verify;
  } else {
    heap->mode = GRHEAP_MODE_COLLECTED;
    grheap_value_codec_default(&heap->codec);
    heap->threshold = GRHEAP_DEFAULT_GC_THRESHOLD;
    heap->auto_collect = true;
    heap->torture = grheap_env_torture();
    heap->verify = grheap_env_verify();
  }
  heap->next_threshold = heap->threshold;

  GRCORE_Result registered = grcore_context_register(context, &grheap_key, heap);
  if (registered != GRCORE_OK) {
    allocator->free_fn(allocator->ctx, heap);
    if (registered == GRCORE_ERR_OOM &&
        grcore_context_memory_refusals(context) > refusals) {
      return GRHEAP_ERR_LIMIT;
    }
    return grheap_from_core(registered);
  }
  *out_heap = heap;
  return GRHEAP_OK;
}

GRHEAP_Heap * grheap_heap_get(const GRCORE_Context * context) {
  if (context == NULL) {
    return NULL;
  }
  return grcore_context_slot(context, &grheap_key);
}

bool grheap_contains(const GRHEAP_Heap * heap, const void * pointer) {
  GRHEAP_Span * span;
  size_t index;
  return grheap_usable(heap) && pointer != NULL &&
      grheap_locate(heap, (uintptr_t)pointer, true, &span, &index);
}

const GRHEAP_Type * grheap_type_of(
    const GRHEAP_Heap * heap, const void * object) {
  if (!grheap_contains(heap, object)) {
    return NULL;
  }
  return grheap_header((void *)(uintptr_t)object)->type;
}

GRHEAP_Result grheap_size_of(
    const GRHEAP_Heap * heap, const void * object, size_t * out_size) {
  if (out_size == NULL || !grheap_contains(heap, object)) {
    return GRHEAP_ERR_INVALID;
  }
  *out_size = (size_t)grheap_header((void *)(uintptr_t)object)->size;
  return GRHEAP_OK;
}

GRHEAP_Result grheap_stats(
    const GRHEAP_Heap * heap, GRHEAP_Stats * out_stats) {
  if (!grheap_usable(heap) || out_stats == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  *out_stats = heap->stats;
  return GRHEAP_OK;
}
