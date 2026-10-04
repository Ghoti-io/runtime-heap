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
 * Stable IDs and the identity hash.
 *
 * Two structures, both in the context's allocator. The *dense array* holds
 * `{id, object}` sorted by ID: IDs are issued in increasing order and never
 * reused, so appending keeps it sorted, and an ID resolves to its object by
 * binary search. The *index* goes the other way, from an object's address to
 * its position in the dense array: an open-addressing table with linear
 * probing and no deletion, because it is rebuilt whenever the array is
 * compacted (a collection that dropped IDs) or grown. The index is the one
 * place in the heap that keys by address, and a moving collector re-keys it
 * (design.md).
 *
 * IDs hold their objects weakly. A collection removes the entry of every
 * unmarked object before the sweep runs its `destroy` hook; the ID is not
 * reused, so a stale ID is simply unknown.
 *
 * Issuing an ID never collects. It grows the tables first and only then takes
 * the next number, so a refusal issues nothing.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#include <stdlib.h>
#include <string.h>

static uint64_t mix(uint64_t x) {
  x += UINT64_C(0x9e3779b97f4a7c15);
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}

static void index_insert(size_t * index, size_t cap, const void * object, size_t pos) {
  size_t mask = cap - 1u;
  size_t at = (size_t)mix((uint64_t)((uintptr_t)object >> 4)) & mask;
  while (index[at] != 0) {
    at = (at + 1u) & mask;
  }
  index[at] = pos + 1u;
}

static void index_rebuild(GRHEAP_Heap * heap) {
  if (heap->id_index_cap == 0) {
    return;
  }
  memset(heap->id_index, 0, heap->id_index_cap * sizeof(size_t));
  for (size_t i = 0; i < heap->nids; i++) {
    index_insert(heap->id_index, heap->id_index_cap, heap->ids[i].object, i);
  }
}

/* The position of `object` in the dense array, or SIZE_MAX. */
static size_t index_find(const GRHEAP_Heap * heap, const void * object) {
  if (heap->id_index_cap == 0) {
    return SIZE_MAX;
  }
  size_t mask = heap->id_index_cap - 1u;
  size_t at = (size_t)mix((uint64_t)((uintptr_t)object >> 4)) & mask;
  while (heap->id_index[at] != 0) {
    size_t pos = heap->id_index[at] - 1u;
    if (heap->ids[pos].object == object) {
      return pos;
    }
    at = (at + 1u) & mask;
  }
  return SIZE_MAX;
}

/* Room for `cap` entries in both structures, `cap` a power of two above the
 * current capacity, or false with nothing changed. The new index is made
 * first: if the array then cannot grow, the index is dropped and the old pair
 * is intact. */
static bool grow_to(GRHEAP_Heap * heap, size_t cap) {
  if (cap > SIZE_MAX / (2u * sizeof(size_t))) {
    return false;
  }
  size_t index_cap = cap * 2u; /* a power of two, at most half full */
  size_t * index =
      heap->allocator->calloc_fn(heap->allocator->ctx, index_cap, sizeof(size_t));
  if (index == NULL) {
    return false;
  }
  GRHEAP_IdEntry * grown = heap->allocator->realloc_fn(
      heap->allocator->ctx, heap->ids, cap * sizeof *grown);
  if (grown == NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, index);
    return false;
  }
  if (heap->id_index != NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, heap->id_index);
  }
  heap->ids = grown;
  heap->ids_cap = cap;
  heap->id_index = index;
  heap->id_index_cap = index_cap;
  index_rebuild(heap);
  return true;
}

/* Room for one more entry in both structures, or false with nothing changed. */
static bool reserve(GRHEAP_Heap * heap) {
  if (heap->nids < heap->ids_cap) {
    return true;
  }
  return grow_to(heap, heap->ids_cap == 0 ? 16u : heap->ids_cap * 2u);
}

uint64_t grheap_id_lookup(const GRHEAP_Heap * heap, const void * object) {
  size_t pos = index_find(heap, object);
  return pos == SIZE_MAX ? 0 : heap->ids[pos].id;
}

bool grheap_ids_reserve_total(GRHEAP_Heap * heap, size_t total) {
  if (total <= heap->ids_cap) {
    return true;
  }
  size_t cap = heap->ids_cap == 0 ? 16u : heap->ids_cap;
  while (cap < total) {
    if (cap > SIZE_MAX / 2u) {
      return false;
    }
    cap *= 2u;
  }
  return grow_to(heap, cap);
}

static int compare_ids(const void * a, const void * b) {
  uint64_t x = ((const GRHEAP_IdEntry *)a)->id;
  uint64_t y = ((const GRHEAP_IdEntry *)b)->id;
  return (x > y) - (x < y);
}

void grheap_ids_adopt(GRHEAP_Heap * heap, size_t count, uint64_t last_id) {
  if (count > 1) {
    qsort(heap->ids, count, sizeof *heap->ids, compare_ids);
  }
  heap->nids = count;
  heap->next_id = last_id;
  heap->stats.ids_issued = last_id;
  heap->stats.ids_live = count;
  index_rebuild(heap);
}

void grheap_ids_unadopt(GRHEAP_Heap * heap) {
  grheap_ids_clear(heap);
  heap->next_id = 0;
  heap->stats.ids_issued = 0;
}

GRHEAP_Result grheap_id_of(
    GRHEAP_Heap * heap, void * object, uint64_t * out_id) {
  GRHEAP_Span * span;
  size_t cell;
  if (!grheap_usable(heap) || object == NULL || out_id == NULL ||
      heap->busy != 0 ||
      !grheap_locate(heap, (uintptr_t)object, true, &span, &cell)) {
    return GRHEAP_ERR_INVALID;
  }
  size_t pos = index_find(heap, object);
  if (pos != SIZE_MAX) {
    *out_id = heap->ids[pos].id;
    return GRHEAP_OK;
  }
  uint64_t refusals = grcore_context_memory_refusals(heap->context);
  if (heap->next_id == UINT64_MAX) {
    return GRHEAP_ERR_LIMIT;
  }
  if (!reserve(heap)) {
    return grheap_failure(heap, refusals);
  }
  uint64_t id = ++heap->next_id;
  heap->ids[heap->nids].id = id;
  heap->ids[heap->nids].object = object;
  index_insert(heap->id_index, heap->id_index_cap, object, heap->nids);
  heap->nids++;
  heap->stats.ids_issued++;
  heap->stats.ids_live++;
  *out_id = id;
  return GRHEAP_OK;
}

GRHEAP_Result grheap_object_of_id(
    const GRHEAP_Heap * heap, uint64_t id, void ** out_object) {
  if (!grheap_usable(heap) || out_object == NULL || id == 0) {
    return GRHEAP_ERR_INVALID;
  }
  size_t lo = 0;
  size_t hi = heap->nids;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    if (heap->ids[mid].id < id) {
      lo = mid + 1u;
    } else if (heap->ids[mid].id > id) {
      hi = mid;
    } else {
      *out_object = heap->ids[mid].object;
      return GRHEAP_OK;
    }
  }
  return GRHEAP_ERR_INVALID;
}

GRHEAP_Result grheap_identity_hash(
    GRHEAP_Heap * heap, void * object, uint64_t * out_hash) {
  uint64_t id;
  if (out_hash == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  GRHEAP_Result r = grheap_id_of(heap, object, &id);
  if (r != GRHEAP_OK) {
    return r;
  }
  *out_hash = mix(id);
  return GRHEAP_OK;
}

void grheap_ids_drop_dead(GRHEAP_Heap * heap) {
  size_t kept = 0;
  for (size_t i = 0; i < heap->nids; i++) {
    GRHEAP_Span * s;
    size_t c;
    if (grheap_locate(heap, (uintptr_t)heap->ids[i].object, true, &s, &c) &&
        grheap_bit_get(s->mark, c)) {
      heap->ids[kept++] = heap->ids[i];
    }
  }
  if (kept != heap->nids) {
    heap->nids = kept;
    index_rebuild(heap);
  }
  heap->stats.ids_live = kept;
}

void grheap_ids_clear(GRHEAP_Heap * heap) {
  heap->nids = 0;
  if (heap->id_index_cap != 0) {
    memset(heap->id_index, 0, heap->id_index_cap * sizeof(size_t));
  }
  heap->stats.ids_live = 0;
}

void grheap_ids_free(GRHEAP_Heap * heap) {
  if (heap->ids != NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, heap->ids);
  }
  if (heap->id_index != NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, heap->id_index);
  }
  heap->ids = NULL;
  heap->id_index = NULL;
  heap->nids = heap->ids_cap = heap->id_index_cap = 0;
}
