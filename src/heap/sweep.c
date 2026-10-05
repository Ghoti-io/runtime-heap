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
 * Sweeping: free what marking did not reach.
 *
 * Every allocated, unmarked cell is dead. Its `destroy` hook runs (with the
 * heap busy, so a hook cannot allocate), the cell goes onto its block's free
 * list, and its allocated bit clears. A block left with no object, and an
 * unmarked large object, are returned to the provider whole. What stays is
 * relinked into the per-class lists of blocks with room, lowest address first,
 * so that allocation fills the blocks it already has before it touches a new
 * one.
 *
 * The dead are found a word of the bit maps at a time: `alloc & ~mark`.
 *
 * Two instruments hook in here. With barrier-verify on, a dead object's shadow
 * entries are forgotten before its `destroy` hook runs. With torture on, every
 * freed cell of a block is poisoned with 0xDD after its free-list link word.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#include <string.h>

/* Frees every dead cell of one block. Returns how many. */
static size_t sweep_block(GRHEAP_Heap * heap, GRHEAP_Span * s) {
  size_t freed = 0;
  size_t words = grheap_bit_words(s->bump);
  for (size_t w = 0; w < words; w++) {
    uint64_t dead = s->alloc[w] & ~s->mark[w];
    for (unsigned b = 0; dead != 0; b++, dead >>= 1) {
      if (!(dead & 1u)) {
        continue;
      }
      size_t index = w * 64u + b;
      void * payload = grheap_payload(s, index);
      const GRHEAP_Type * type = grheap_header(payload)->type;
      if (heap->verify != GRHEAP_VERIFY_OFF) {
        /* Before `destroy`, so a hook sees no stale shadow of its own slots,
         * and while whatever the object points at is still there to be
         * walked. The cell's own range goes first; then every slot the
         * object's trace reports, which includes slots that are not in the
         * cell (a buffer the object owns), whose shadow would otherwise
         * outlive it and be compared with whatever reuses that memory. */
        grheap_shadow_forget_range(heap, (uintptr_t)payload,
            (size_t)(s->cell - GRHEAP_HEADER_BYTES));
        if (type->trace != NULL) {
          GRHEAP_Tracer tracer = {.heap = heap,
              .mode = GRHEAP_TRACE_FORGET,
              .payload = payload,
              .size = (size_t)(s->cell - GRHEAP_HEADER_BYTES)};
          type->trace(&tracer, payload);
        }
      }
      if (type->destroy != NULL) {
        type->destroy(payload);
      }
      void * cell = (unsigned char *)payload - GRHEAP_HEADER_BYTES;
      grheap_bit_clear(s->alloc, index);
      *(void **)cell = s->free_list; /* overwrites the dead header's type */
      s->free_list = cell;
      if (heap->torture && !s->large) {
        /* Everything after the link word, so that a read through a dangling
         * pointer sees 0xDD and not plausible data. ASan cannot see inside a
         * block, so this is the instrument. (The link word *is* the type word;
         * nothing else of the cell survives.) */
        memset((unsigned char *)cell + sizeof(void *), 0xDD,
            s->cell - sizeof(void *));
      }
      freed++;
    }
  }
  s->live -= freed;
  return freed;
}

void grheap_sweep(GRHEAP_Heap * heap) {
  GRHEAP_Span * tails[GRHEAP_CLASS_COUNT];
  for (unsigned c = 0; c < GRHEAP_CLASS_COUNT; c++) {
    heap->avail[c] = NULL;
    tails[c] = NULL;
  }
  uint64_t live_objects = 0;
  uint64_t live_bytes = 0;
  uint64_t freed = 0;
  size_t kept = 0;

  heap->busy++;
  /* First every dead cell, hooks included, with the span array exactly as it
   * was. A hook may ask the heap a question (contains, type_of, size_of), and
   * the array must hold only spans that still exist: compacting it and
   * freeing emptied spans in the same loop left the hook of a later span
   * searching entries that pointed at freed spans. */
  for (size_t i = 0; i < heap->nspans; i++) {
    freed += sweep_block(heap, heap->spans[i]);
  }
  /* Then the spans: clear the marks, return what emptied, relink what is left. */
  for (size_t i = 0; i < heap->nspans; i++) {
    GRHEAP_Span * s = heap->spans[i];
    memset(s->mark, 0, grheap_bit_words(s->ncells) * sizeof(uint64_t));
    if (s->live == 0) {
      grheap_span_free(heap, s);
      continue;
    }
    live_objects += s->live;
    live_bytes += (uint64_t)s->live * s->cell;
    heap->spans[kept++] = s;
    if (!s->large && (s->free_list != NULL || s->bump < s->ncells)) {
      s->next_avail = NULL;
      if (tails[s->cls] == NULL) {
        heap->avail[s->cls] = s;
      } else {
        tails[s->cls]->next_avail = s;
      }
      tails[s->cls] = s;
    }
  }
  heap->nspans = kept;
  heap->busy--;

  heap->stats.live_objects = live_objects;
  heap->stats.live_bytes = live_bytes;
  heap->stats.frees += freed;
}
