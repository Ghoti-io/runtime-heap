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
 * Spans: the heap's mappings, the sorted array that finds one by address, the
 * size classes, and the resolution of an address to a cell.
 *
 * The array is sorted by base address and searched in O(log n), which is what
 * lets a conservative scan decide whether a word names an object without
 * walking the heap. Insertion is O(n) in the number of spans, which only a
 * block or a large mapping pays; an allocation that fits an existing block
 * never touches the array.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#include <string.h>

bool grheap_cell_size(size_t payload, size_t * out_cell) {
  /* header + payload + one pad byte, rounded up to 16. The bound leaves room
   * for the three additions and the rounding. */
  if (payload > SIZE_MAX - 2u * GRHEAP_HEADER_BYTES) {
    return false;
  }
  *out_cell = grheap_cell_of_size(payload);
  return true;
}

/* floor(log2(n)) for n > 0. */
static unsigned log2_floor(size_t n) {
#if defined(__GNUC__) || defined(__clang__)
  return (unsigned)(sizeof(unsigned long long) * 8u - 1u -
      (unsigned)__builtin_clzll((unsigned long long)n));
#else
  unsigned r = 0;
  while (n >>= 1) {
    r++;
  }
  return r;
#endif
}

unsigned grheap_class_of(size_t cell, size_t * out_class_cell) {
  if (cell <= 128u) {
    /* 32, 48, ... 128 are classes 0 to 6. */
    *out_class_cell = cell;
    return (unsigned)(cell / 16u) - 2u;
  }
  /* Four classes per doubling: for a cell in (2^hi, 2^(hi+1)] the step is
   * 2^hi / 4, and the class is the cell rounded up to a multiple of it. */
  unsigned hi = log2_floor(cell - 1u);
  size_t step = (size_t)1 << (hi - 2u);
  size_t rounded = (cell + step - 1u) & ~(step - 1u);
  *out_class_cell = rounded;
  return 7u + (hi - 7u) * 4u + (unsigned)((rounded - ((size_t)1 << hi)) / step) -
      1u;
}

GRHEAP_Span * grheap_span_find(const GRHEAP_Heap * heap, uintptr_t addr) {
  size_t lo = 0;
  size_t hi = heap->nspans;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    GRHEAP_Span * s = heap->spans[mid];
    if (addr < s->base) {
      hi = mid;
    } else if (addr - s->base >= s->size) {
      lo = mid + 1u;
    } else {
      return s;
    }
  }
  return NULL;
}

/* Makes room for one more entry. Called before anything else is acquired, so
 * that the insert after the pages are mapped cannot fail. */
static bool reserve_span_slot(GRHEAP_Heap * heap) {
  if (heap->nspans < heap->spans_cap) {
    return true;
  }
  size_t cap = heap->spans_cap == 0 ? 16u : heap->spans_cap * 2u;
  if (cap > SIZE_MAX / sizeof(GRHEAP_Span *)) {
    return false;
  }
  GRHEAP_Span ** grown = heap->allocator->realloc_fn(
      heap->allocator->ctx, heap->spans, cap * sizeof(GRHEAP_Span *));
  if (grown == NULL) {
    return false;
  }
  heap->spans = grown;
  heap->spans_cap = cap;
  return true;
}

static void insert_span(GRHEAP_Heap * heap, GRHEAP_Span * span) {
  size_t lo = 0;
  size_t hi = heap->nspans;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (heap->spans[mid]->base < span->base) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  memmove(&heap->spans[lo + 1u], &heap->spans[lo],
      (heap->nspans - lo) * sizeof(GRHEAP_Span *));
  heap->spans[lo] = span;
  heap->nspans++;
}

GRHEAP_Span * grheap_span_create(
    GRHEAP_Heap * heap, GRHEAP_SpanKind kind, size_t cell, bool large) {
  size_t map_size;
  size_t ncells;
  if (large) {
    /* One page-rounded mapping per oversized object. */
    size_t page = heap->pages->page_size;
    if (cell > SIZE_MAX - page) {
      return NULL;
    }
    map_size = (cell + page - 1u) / page * page;
    ncells = 1;
  } else {
    map_size = heap->block_size;
    ncells = kind == GRHEAP_SPAN_ARENA ? map_size / 16u : map_size / cell;
  }
  size_t words = grheap_bit_words(ncells);
#ifdef GRHEAP_RELOCATE
  size_t maps = kind == GRHEAP_SPAN_BLOCK ? 3u : 1u;
#else
  size_t maps = kind == GRHEAP_SPAN_BLOCK ? 2u : 1u;
#endif

  if (!reserve_span_slot(heap)) {
    return NULL;
  }
  GRHEAP_Span * span = heap->allocator->calloc_fn(heap->allocator->ctx, 1,
      sizeof(GRHEAP_Span) + maps * words * sizeof(uint64_t));
  if (span == NULL) {
    return NULL;
  }
  void * mem = heap->pages->map(heap->pages->ctx, map_size);
  if (mem == NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, span);
    return NULL;
  }
  span->base = (uintptr_t)mem;
  span->size = map_size;
  span->kind = kind;
  span->large = large;
  span->cell = kind == GRHEAP_SPAN_BLOCK ? cell : 0;
  span->ncells = ncells;
  span->alloc = span->bits;
  span->mark = kind == GRHEAP_SPAN_BLOCK ? span->bits + words : NULL;
#ifdef GRHEAP_RELOCATE
  span->stay = kind == GRHEAP_SPAN_BLOCK ? span->bits + 2u * words : NULL;
#endif
  insert_span(heap, span);
  heap->stats.mapped_bytes += map_size;
  return span;
}

void grheap_span_free(GRHEAP_Heap * heap, GRHEAP_Span * span) {
  heap->pages->unmap(heap->pages->ctx, (void *)span->base, span->size);
  heap->stats.mapped_bytes -= span->size;
  if (span->pins != NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, span->pins);
  }
  heap->allocator->free_fn(heap->allocator->ctx, span);
}

bool grheap_locate(const GRHEAP_Heap * heap, uintptr_t addr, bool exact,
    GRHEAP_Span ** out_span, size_t * out_index) {
  GRHEAP_Span * s = grheap_span_find(heap, addr);
  if (s == NULL) {
    return false;
  }
  size_t index;
  if (s->kind == GRHEAP_SPAN_ARENA) {
    /* An arena is never scanned conservatively, so only an exact payload
     * start has a meaning: the granule 16 bytes before it starts a cell. */
    if (!exact || addr - s->base < GRHEAP_HEADER_BYTES) {
      return false;
    }
    size_t off = addr - s->base - GRHEAP_HEADER_BYTES;
    if (off % 16u != 0 || off >= s->bump) {
      return false;
    }
    index = off / 16u;
  } else {
    size_t off = addr - s->base;
    index = off / s->cell;
    if (index >= s->ncells) {
      return false; /* the tail of a mapping, past the last whole cell */
    }
    if (exact && off - index * s->cell != GRHEAP_HEADER_BYTES) {
      return false;
    }
  }
  if (!grheap_bit_get(s->alloc, index)) {
    return false;
  }
  *out_span = s;
  *out_index = index;
  return true;
}
