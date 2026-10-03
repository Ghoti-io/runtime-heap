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
 * Allocation: taking a cell for an object, and the public `grheap_alloc`.
 *
 * Small objects come from blocks of one size class, large ones from a mapping
 * of their own. Cells are zero-filled: a cell off a free list is cleared here,
 * and a cell from the bump index is still the zero page the provider gave.
 *
 * An allocation is a GC point (AD-17), and so is allowed to collect: first
 * when the bytes allocated since the last collection reach the threshold, and
 * then once, with a retry, when a block or mapping cannot be had. Whatever
 * the failure, nothing is left half done: a span is created whole or not at
 * all, and the cell is taken only after the span exists.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#include <string.h>

GRHEAP_Result grheap_from_core(GRCORE_Result result) {
  /* The two enums share their numbers up to INTERNAL. Anything else (GUEST is
   * the one value runtime-core adds) is not something a heap call can get
   * back from a registration. */
  if ((int)result >= (int)GRHEAP_RESULT_COUNT || (int)result < 0) {
    return GRHEAP_ERR_INTERNAL;
  }
  return (GRHEAP_Result)result;
}

GRHEAP_Result grheap_failure(
    const GRHEAP_Heap * heap, uint64_t refusals_before) {
  return grcore_context_memory_refusals(heap->context) > refusals_before
      ? GRHEAP_ERR_LIMIT
      : GRHEAP_ERR_OOM;
}

/* A cell of a small class: off the class's list of blocks with room. */
static void * take_small(GRHEAP_Heap * heap, size_t cell) {
  size_t class_cell;
  unsigned cls = grheap_class_of(cell, &class_cell);
  GRHEAP_Span * s = heap->avail[cls];
  if (s == NULL) {
    s = grheap_span_create(heap, GRHEAP_SPAN_BLOCK, class_cell, false);
    if (s == NULL) {
      return NULL;
    }
    s->cls = cls;
    s->next_avail = NULL;
    heap->avail[cls] = s;
  }
  size_t index;
  void * cellp;
  if (s->free_list != NULL) {
    cellp = s->free_list;
    s->free_list = *(void **)cellp;
    index = ((uintptr_t)cellp - s->base) / s->cell;
    memset(cellp, 0, s->cell);
  } else {
    index = s->bump++;
    cellp = (void *)(s->base + index * s->cell);
  }
  grheap_bit_set(s->alloc, index);
  s->live++;
  if (s->free_list == NULL && s->bump == s->ncells) {
    heap->avail[cls] = s->next_avail; /* full: off the list */
  }
  return cellp;
}

/* A mapping for one oversized object. */
static void * take_large(GRHEAP_Heap * heap, size_t cell) {
  GRHEAP_Span * s = grheap_span_create(heap, GRHEAP_SPAN_BLOCK, cell, true);
  if (s == NULL) {
    return NULL;
  }
  s->cell = cell;
  s->bump = 1;
  s->live = 1;
  grheap_bit_set(s->alloc, 0);
  return (void *)s->base;
}

void * grheap_take_cell(GRHEAP_Heap * heap, size_t cell) {
  if (cell > GRHEAP_SMALL_CELL_MAX) {
    return take_large(heap, cell);
  }
  return take_small(heap, cell);
}

static GRHEAP_Result alloc_object(GRHEAP_Heap * heap, const GRHEAP_Type * type,
    size_t bytes, void ** out_object) {
  if (!grheap_usable(heap) || type == NULL || out_object == NULL ||
      heap->busy != 0) {
    return GRHEAP_ERR_INVALID;
  }
  if (type->weak != NULL && !grheap_weak_spec_fits(type, bytes)) {
    return GRHEAP_ERR_INVALID; /* a weak slot outside the payload */
  }
  size_t cell;
  if (!grheap_cell_size(bytes, &cell)) {
    return GRHEAP_ERR_OOM;
  }
  uint64_t refusals = grcore_context_memory_refusals(heap->context);
  void * cellp;
  bool weak = type->weak != NULL && heap->mode == GRHEAP_MODE_COLLECTED;
  if (weak && !grheap_weak_reserve(heap)) {
    /* The registry grows before the cell is taken, so a failure here has
     * taken nothing. */
    return grheap_failure(heap, refusals);
  }

  if (heap->mode == GRHEAP_MODE_ARENA) {
    cellp = grheap_arena_take(heap, cell);
    if (cellp == NULL) {
      return grheap_failure(heap, refusals);
    }
  } else {
    bool collected = false;
    if (heap->torture) {
      /* A GC point (AD-17) taken for real, every time: whatever is held only
       * in a C variable is gone now, and its cell is poisoned. */
      heap->stats.torture_collections++;
      grheap_collect_now(heap);
      collected = true;
      refusals = grcore_context_memory_refusals(heap->context);
    } else if (heap->auto_collect &&
        heap->since_collect >= heap->next_threshold) {
      grheap_collect_now(heap);
      collected = true;
      /* The collection allocates (its mark stack), and the budget may have
       * refused that; a refusal there is not why the cell below fails. */
      refusals = grcore_context_memory_refusals(heap->context);
    }
    cellp = grheap_take_cell(heap, cell);
    if (cellp == NULL && !collected) {
      /* A block or a mapping could not be had: collect once, and try again.
       * What it frees may be whole blocks, handed back to the provider. */
      grheap_collect_now(heap);
      refusals = grcore_context_memory_refusals(heap->context);
      cellp = grheap_take_cell(heap, cell);
    }
    if (cellp == NULL) {
      return grheap_failure(heap, refusals);
    }
  }

  GRHEAP_Header * header = cellp;
  header->type = type;
  header->size = bytes;
  heap->stats.allocations++;
  heap->stats.live_objects++;
  /* The bytes a sweep will count: a small object occupies its class's cell,
   * which can be larger than the 16-byte-rounded cell asked for. */
  size_t charged = cell;
  if (cell <= GRHEAP_SMALL_CELL_MAX) {
    (void)grheap_class_of(cell, &charged);
  }
  heap->stats.live_bytes += charged;
  heap->since_collect += charged;
  if (weak) {
    grheap_weak_register(heap, (unsigned char *)cellp + GRHEAP_HEADER_BYTES);
  }
  *out_object = (unsigned char *)cellp + GRHEAP_HEADER_BYTES;
  return GRHEAP_OK;
}

GRHEAP_Result grheap_alloc(
    GRHEAP_Heap * heap, const GRHEAP_Type * type, void ** out_object) {
  if (type == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  return alloc_object(heap, type, type->size, out_object);
}

GRHEAP_Result grheap_alloc_sized(GRHEAP_Heap * heap, const GRHEAP_Type * type,
    size_t bytes, void ** out_object) {
  return alloc_object(heap, type, bytes, out_object);
}
