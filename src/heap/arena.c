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
 * Arena mode, and the teardown it shares with the collected heap.
 *
 * An arena bump allocates from chunks the size of a block, and never frees an
 * object on its own. Objects too big for a chunk's cells get a mapping each,
 * as in collected mode. `grheap_arena_reset` runs every `destroy` hook and
 * releases every chunk, keeping the heap; destroying the context does the same
 * and then frees the heap itself.
 *
 * A chunk is walked from its base by the sizes in the object headers, since
 * the cells follow one another with no gap. That is how the hooks are found
 * without a table of objects.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

void * grheap_arena_take(GRHEAP_Heap * heap, size_t cell) {
  if (cell > GRHEAP_SMALL_CELL_MAX) {
    return grheap_take_cell(heap, cell); /* a large mapping */
  }
  GRHEAP_Span * s = heap->arena_cur;
  if (s == NULL || s->size - s->bump < cell) {
    s = grheap_span_create(heap, GRHEAP_SPAN_ARENA, 0, false);
    if (s == NULL) {
      return NULL;
    }
    heap->arena_cur = s;
  }
  size_t offset = s->bump;
  s->bump += cell;
  s->live++;
  grheap_bit_set(s->alloc, offset / 16u);
  return (void *)(s->base + offset);
}

static void destroy_payload(void * payload) {
  GRHEAP_Header * header = grheap_header(payload);
  if (header->type != NULL && header->type->destroy != NULL) {
    header->type->destroy(payload);
  }
}

void grheap_run_all_destroys(GRHEAP_Heap * heap) {
  heap->busy++;
  for (size_t i = 0; i < heap->nspans; i++) {
    GRHEAP_Span * s = heap->spans[i];
    if (s->kind == GRHEAP_SPAN_ARENA) {
      size_t offset = 0;
      while (offset < s->bump) {
        void * payload = (void *)(s->base + offset + GRHEAP_HEADER_BYTES);
        size_t cell = grheap_cell_of_size(grheap_header(payload)->size);
        destroy_payload(payload);
        offset += cell;
      }
    } else {
      for (size_t c = 0; c < s->bump; c++) {
        if (grheap_bit_get(s->alloc, c)) {
          destroy_payload(grheap_payload(s, c));
        }
      }
    }
  }
  heap->busy--;
}

void grheap_release_all_spans(GRHEAP_Heap * heap) {
  for (size_t i = 0; i < heap->nspans; i++) {
    grheap_span_free(heap, heap->spans[i]);
  }
  heap->nspans = 0;
  heap->arena_cur = NULL;
  for (unsigned c = 0; c < GRHEAP_CLASS_COUNT; c++) {
    heap->avail[c] = NULL;
  }
}

GRHEAP_Result grheap_arena_reset(GRHEAP_Heap * heap) {
  if (!grheap_usable(heap) || heap->mode != GRHEAP_MODE_ARENA ||
      heap->busy != 0) {
    return GRHEAP_ERR_INVALID;
  }
  grheap_run_all_destroys(heap);
  grheap_release_all_spans(heap);
  grheap_roots_clear_for_reset(heap);
  grheap_ids_clear(heap);
  grheap_shadow_clear(heap);
  heap->stats.frees += heap->stats.live_objects;
  heap->stats.live_objects = 0;
  heap->stats.live_bytes = 0;
  heap->stats.pins = 0;
  heap->since_collect = 0;
  return GRHEAP_OK;
}
