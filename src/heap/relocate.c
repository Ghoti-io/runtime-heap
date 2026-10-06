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
 * Relocation torture: the test-only mode that moves objects at every
 * collection. This file is compiled only into a build made with
 * `RELOCATE=yes` (the Makefile leaves it out of every other build, and
 * `make check-no-relocation` fails if its code reaches one). See relocate.h
 * for the contract and design.md for the decision and what it rejected.
 *
 * The phase runs inside a collection, after marking, weak processing and the
 * dropping of dead IDs, and before the sweep. Everything that is marked,
 * that no conservative range hit and that no pin holds is *moved*:
 *
 *   1. Choose the objects, in address order, into a table of `{from, to}`.
 *      No table, no relocation this collection: nothing is half done.
 *   2. Take a new cell for each. An object whose cell cannot be had stays.
 *   3. Rewrite every reference the collector was shown, by the new
 *      addresses: root slots, handles, the root sources' precise slots, and
 *      every precise and weak slot of every marked object. This is done in
 *      the *old* cells, before they are copied, so that the copy carries the
 *      rewritten slots and a cycle or a self-reference needs no special case.
 *   4. Copy each old cell to its new one and mark the new one.
 *   5. Re-address the tables: the weak registry, the ID table and its address
 *      index, and the shadow table.
 *   6. Free every old cell and poison it (0xDD after its free-list link),
 *      without running its `destroy`: the object lives on.
 *
 * A reference that was visited by the mark phase but not rewritten here (a
 * slot reported through a copy, a field `trace` leaves out, an address kept as
 * a key) still names the old cell, and the next read through it sees poison.
 * Poison is the whole detector; nothing here checks anything.
 *
 * Old cells are not reused until every object has moved, so no object lands
 * where another was: a stale reference never reads plausible data. A large
 * object's old mapping is returned to the provider whole, as a swept one is,
 * and a read through a stale reference to it faults instead.
 *
 * The `GRHEAP_PLANT_RELOC_*` macros are planted defects, each leaving one table
 * or slot kind un-updated. They exist for `make check-relocation-gates`, which
 * builds the library with one at a time and requires tests/unit/test_relocate
 * to fail. Nothing defines them otherwise.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#ifdef GRHEAP_RELOCATE

#include <string.h>

/** The byte a freed cell is filled with, as the sweep's torture does. */
#define POISON 0xDD

GRHEAP_Result grheap_relocation_stats(
    const GRHEAP_Heap * heap, GRHEAP_RelocationStats * out_stats) {
  if (!grheap_usable(heap) || out_stats == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  *out_stats = heap->reloc_stats;
  return GRHEAP_OK;
}

void grheap_reloc_stay(GRHEAP_Span * span, size_t index) {
  if (span->stay != NULL) {
    grheap_bit_set(span->stay, index);
  }
}

void * grheap_reloc_forward(const GRHEAP_Heap * heap, const void * object) {
  uintptr_t key = (uintptr_t)object;
  size_t lo = 0;
  size_t hi = heap->reloc_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2u;
    uintptr_t from = heap->reloc_moves[mid].from;
    if (from < key) {
      lo = mid + 1u;
    } else if (from > key) {
      hi = mid;
    } else {
      uintptr_t to = heap->reloc_moves[mid].to;
      return to != 0 ? (void *)to : (void *)(uintptr_t)key;
    }
  }
  return (void *)(uintptr_t)key;
}

/* The word that names `address` in place of the one a slot holds: the same
 * bits everywhere but the address field (and the bits of that field below the
 * shift, which the decode drops and so the encoding keeps). */
static uint64_t encode(const GRHEAP_Heap * heap, uint64_t old_word, uint64_t address) {
  const GRHEAP_ValueCodec * c = &heap->codec;
  uint64_t low = c->shift == 0 ? 0 : (UINT64_C(1) << c->shift) - 1u;
  uint64_t field = c->mask & ~low;
  return (old_word & ~field) | (((address - c->base) << c->shift) & field);
}

void grheap_reloc_slot(GRHEAP_Tracer * tracer, void * slot, bool encoded) {
  GRHEAP_Heap * heap = tracer->heap;
  uint64_t old_word;
  memcpy(&old_word, slot, sizeof old_word);
  uint64_t address = old_word;
  if (encoded && !grheap_decode(heap, old_word, &address)) {
    return; /* an immediate, not a reference */
  }
  if (address == 0 || (uint64_t)(uintptr_t)address != address) {
    return;
  }
  uintptr_t to = (uintptr_t)grheap_reloc_forward(heap, (void *)(uintptr_t)address);
  if (to == (uintptr_t)address) {
    return;
  }
  uint64_t new_word = encoded ? encode(heap, old_word, to) : (uint64_t)to;
  memcpy(slot, &new_word, sizeof new_word);
  if (heap->verify != GRHEAP_VERIFY_OFF) {
    grheap_shadow_update(heap, (uintptr_t)slot, old_word, new_word);
  }
}

/* The precise slots a root source reports. */
static void visit_root_slot(void * user, uint64_t * slot) {
  GRHEAP_Tracer tracer = {.heap = user, .mode = GRHEAP_TRACE_RELOCATE};
  grheap_reloc_slot(&tracer, slot, true);
}

typedef struct WeakWalk {
  GRHEAP_Tracer * tracer;
  bool encoded;
} WeakWalk;

static void weak_slot(void * user, const void * slot) {
  WeakWalk * w = user;
  grheap_reloc_slot(w->tracer, (void *)(uintptr_t)slot, w->encoded);
}

/* Every precise and weak slot of one marked object. */
static void rewrite_object(GRHEAP_Heap * heap, void * payload) {
  const GRHEAP_Type * type = grheap_header(payload)->type;
  GRHEAP_Tracer tracer = {.heap = heap, .mode = GRHEAP_TRACE_RELOCATE};
  if (type->trace != NULL) {
    type->trace(&tracer, payload);
  }
#ifndef GRHEAP_PLANT_RELOC_WEAKSLOT_STALE
  const GRHEAP_WeakSpec * weak = grheap_type_weak(type);
  if (weak != NULL) {
    WeakWalk w = {&tracer, weak->encoded};
    grheap_weak_each_slot(type, payload, weak_slot, &w);
  }
#endif
}

/* The marked objects that may move, counted or recorded in address order, and
 * the reasons the others stay. */
static size_t choose(GRHEAP_Heap * heap, GRHEAP_RelocMove * out) {
  size_t n = 0;
  for (size_t i = 0; i < heap->nspans; i++) {
    GRHEAP_Span * s = heap->spans[i];
    if (s->kind != GRHEAP_SPAN_BLOCK) {
      continue;
    }
    for (size_t c = 0; c < s->bump; c++) {
      if (!grheap_bit_get(s->alloc, c) || !grheap_bit_get(s->mark, c)) {
        continue;
      }
      bool pinned = s->pins != NULL && s->pins[c] != 0;
      bool held = grheap_bit_get(s->stay, c);
#ifdef GRHEAP_PLANT_RELOC_MOVE_PINNED
      pinned = false;
      held = false;
#endif
      if (pinned || held) {
        if (out == NULL) {
          if (pinned) {
            heap->reloc_stats.stayed_pinned++;
          } else {
            heap->reloc_stats.stayed_conservative++;
          }
        }
        continue;
      }
      if (out != NULL) {
        out[n].from = (uintptr_t)grheap_payload(s, c);
        out[n].to = 0;
      }
      n++;
    }
  }
  return n;
}

/* Copies the object to its new cell and marks that cell. */
static void copy_object(GRHEAP_Heap * heap, const GRHEAP_RelocMove * m) {
  GRHEAP_Span * os;
  size_t oi;
  GRHEAP_Span * ns;
  size_t ni;
  if (!grheap_locate(heap, m->from, true, &os, &oi) ||
      !grheap_locate(heap, m->to, true, &ns, &ni)) {
    return; /* cannot happen: both cells were located when they were chosen */
  }
  memcpy((unsigned char *)(uintptr_t)m->to - GRHEAP_HEADER_BYTES,
      (unsigned char *)(uintptr_t)m->from - GRHEAP_HEADER_BYTES, os->cell);
  grheap_bit_set(ns->mark, ni);
  heap->reloc_stats.moved++;
  heap->reloc_stats.moved_bytes += os->cell;
}

/* Frees the old cell like a swept one, but runs no `destroy`. */
static void free_old(GRHEAP_Heap * heap, const GRHEAP_RelocMove * m) {
  GRHEAP_Span * s;
  size_t i;
  if (!grheap_locate(heap, m->from, true, &s, &i)) {
    return;
  }
  void * cell = (unsigned char *)(uintptr_t)m->from - GRHEAP_HEADER_BYTES;
  grheap_bit_clear(s->alloc, i);
  grheap_bit_clear(s->mark, i);
  s->live--;
  if (s->large) {
    return; /* the sweep returns the whole mapping, as for a dead one */
  }
  *(void **)cell = s->free_list;
  s->free_list = cell;
  memset((unsigned char *)cell + sizeof(void *), POISON, s->cell - sizeof(void *));
}

void grheap_reloc_run(GRHEAP_Heap * heap) {
  if (heap->pending != NULL) {
    /* A restore between APPLY and COMMIT holds the addresses of the objects
     * it made; nothing moves under it. */
    heap->reloc_stats.skipped++;
    return;
  }
  size_t n = choose(heap, NULL);
  heap->reloc_stats.collections++;
  if (n == 0) {
    return;
  }
  if (n > SIZE_MAX / sizeof(GRHEAP_RelocMove)) {
    heap->reloc_stats.skipped++;
    return;
  }
  GRHEAP_RelocMove * moves =
      heap->allocator->malloc_fn(heap->allocator->ctx, n * sizeof *moves);
  if (moves == NULL) {
    heap->reloc_stats.skipped++;
    return;
  }
  (void)choose(heap, moves);
  heap->reloc_moves = moves;
  heap->reloc_count = n;

  /* 2. New cells. Taking one can make a span, which changes the span array,
   * so nothing here walks it. */
  for (size_t k = 0; k < n; k++) {
    size_t cell = grheap_cell_of_size(grheap_header((void *)moves[k].from)->size);
    void * cellp = grheap_take_cell(heap, cell);
    if (cellp == NULL) {
      moves[k].to = 0;
      heap->reloc_stats.stayed_no_cell++;
    } else {
      moves[k].to = (uintptr_t)cellp + GRHEAP_HEADER_BYTES;
    }
  }

  /* 3. Every reference the collector was shown. */
#ifndef GRHEAP_PLANT_RELOC_ROOT_STALE
  for (size_t i = 0; i < heap->nroots; i++) {
    *heap->roots[i] = grheap_reloc_forward(heap, *heap->roots[i]);
  }
  for (struct GRHEAP_Handle * h = heap->handles; h != NULL; h = h->next) {
    h->object = grheap_reloc_forward(heap, h->object);
  }
#endif
#ifndef GRHEAP_PLANT_RELOC_SOURCE_STALE
  GRCORE_RootVisitor visitor = {heap, visit_root_slot, NULL};
  grcore_context_enumerate_roots(heap->context, &visitor);
#endif
  for (size_t i = 0; i < heap->nspans; i++) {
    GRHEAP_Span * s = heap->spans[i];
    if (s->kind != GRHEAP_SPAN_BLOCK) {
      continue;
    }
    for (size_t c = 0; c < s->bump; c++) {
      if (grheap_bit_get(s->alloc, c) && grheap_bit_get(s->mark, c)) {
        rewrite_object(heap, grheap_payload(s, c));
      }
    }
  }

  /* 4. Copy, 5. the tables, 6. free. */
  for (size_t k = 0; k < n; k++) {
    if (moves[k].to != 0) {
      copy_object(heap, &moves[k]);
    }
  }
  grheap_weak_relocate(heap);
  grheap_ids_relocate(heap);
  for (size_t k = 0; k < n; k++) {
    if (moves[k].to != 0) {
      GRHEAP_Span * s;
      size_t i;
      if (grheap_locate(heap, moves[k].from, true, &s, &i)) {
        grheap_shadow_relocate_cell(heap, moves[k].from, moves[k].to,
            s->cell - GRHEAP_HEADER_BYTES);
      }
    }
  }
  for (size_t k = 0; k < n; k++) {
    if (moves[k].to != 0) {
      free_old(heap, &moves[k]);
    }
  }

  heap->reloc_moves = NULL;
  heap->reloc_count = 0;
  heap->allocator->free_fn(heap->allocator->ctx, moves);
}

#endif /* GRHEAP_RELOCATE */
