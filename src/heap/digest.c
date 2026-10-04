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
 * A digest of everything a read-only operation must leave alone.
 *
 * It exists for the tests of operations that promise to change nothing (the
 * retention query): a count of public statistics cannot see a mark bit left
 * set, a pin count touched or a free list reordered, and each of those changes
 * what the next collection does. The digest reads every span's allocation,
 * mark and pin bits, its free list, bump index and counts, the mark stack's
 * bookkeeping, the counters, the thresholds and the ID tables' sizes, and folds
 * them into one 64-bit FNV-1a value. It is `GRHEAP_INTERNAL_API`, so it is
 * visible to the tests that link the archive and hidden in the shipped library.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

static uint64_t fold(uint64_t h, uint64_t value) {
  for (unsigned i = 0; i < 8u; i++) {
    h = (h ^ ((value >> (i * 8u)) & 0xFFu)) * UINT64_C(0x100000001B3);
  }
  return h;
}

uint64_t grheap_state_digest(const GRHEAP_Heap * heap) {
  uint64_t h = UINT64_C(0xCBF29CE484222325);
  const unsigned char * stats = (const unsigned char *)&heap->stats;
  for (size_t i = 0; i < sizeof heap->stats; i++) {
    h = fold(h, stats[i]);
  }
  h = fold(h, heap->busy);
  h = fold(h, heap->mark_top);
  h = fold(h, heap->mark_cap);
  h = fold(h, heap->mark_overflow);
  h = fold(h, heap->since_collect);
  h = fold(h, heap->next_threshold);
  h = fold(h, heap->nroots);
  h = fold(h, heap->nweak);
  h = fold(h, heap->nids);
  h = fold(h, heap->next_id);
  h = fold(h, heap->shadow_count);
  h = fold(h, heap->nspans);
  for (size_t i = 0; i < heap->nspans; i++) {
    const GRHEAP_Span * s = heap->spans[i];
    h = fold(h, s->base);
    h = fold(h, s->bump);
    h = fold(h, s->live);
    h = fold(h, s->npinned);
    h = fold(h, (uint64_t)(uintptr_t)s->free_list);
    size_t words = grheap_bit_words(s->ncells);
    for (size_t w = 0; w < words; w++) {
      h = fold(h, s->alloc[w]);
      if (s->mark != NULL) {
        h = fold(h, s->mark[w]);
      }
    }
    if (s->pins != NULL) {
      for (size_t c = 0; c < s->ncells; c++) {
        h = fold(h, s->pins[c]);
      }
    }
  }
  return h;
}
