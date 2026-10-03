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
 * Weak cells and ephemerons.
 *
 * A type with a `weak` spec (type.h) has one weak slot or one key and value
 * slot, which its `trace` function does not report. The heap keeps a registry
 * of such objects, appended to at allocation (in an array grown *before* the
 * cell is taken, so an allocation that fails changes nothing), and handles
 * them between marking and sweeping, so that a collection runs:
 *
 *   mark -> ephemeron fixpoint -> clear weak slots -> drop dead cells and IDs
 *        -> sweep -> `cleared` callbacks
 *
 * The fixpoint marks the value of every live ephemeron whose key is marked,
 * which can mark another ephemeron's key, so it repeats until a pass marks
 * nothing. Only then is a weak slot judged: a weak cell must never see a
 * target that an ephemeron value was about to keep. A slot that is cleared is
 * stored as zero, and its object is flagged *pending*, which is the callback
 * queue: it lives in the registry entry, so the collector allocates nothing.
 * The callbacks run after the sweep, with the heap busy, so they only ever see
 * live cells and cannot disturb the free lists.
 *
 * Arena mode never collects, so nothing here ever runs for an arena.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#include <string.h>

/* How a weak slot reads. */
typedef enum {
  REF_NULL,      ///< Empty.
  REF_IMMEDIATE, ///< An encoded word that is not a pointer.
  REF_INVALID,   ///< Not the start of a live payload.
  REF_LIVE       ///< The start of a live payload.
} RefState;

static uint64_t load(const unsigned char * payload, size_t offset) {
  uint64_t v;
  memcpy(&v, payload + offset, sizeof v);
  return v;
}

static RefState classify(const GRHEAP_Heap * heap, const GRHEAP_WeakSpec * spec,
    const unsigned char * payload, size_t offset, GRHEAP_Span ** out_span,
    size_t * out_index) {
  uint64_t address = load(payload, offset);
  if (spec->encoded && !grheap_decode(heap, address, &address)) {
    return REF_IMMEDIATE;
  }
  if (address == 0) {
    return REF_NULL;
  }
  if ((uint64_t)(uintptr_t)address != address ||
      !grheap_locate(heap, (uintptr_t)address, true, out_span, out_index)) {
    return REF_INVALID;
  }
  return REF_LIVE;
}

static bool marked(const GRHEAP_Heap * heap, const void * payload) {
  GRHEAP_Span * s;
  size_t i;
  return grheap_locate(heap, (uintptr_t)payload, true, &s, &i) &&
      grheap_bit_get(s->mark, i);
}

/* Stores zero in a slot the collector is clearing, and tells the shadow. */
static void clear_slot(GRHEAP_Heap * heap, unsigned char * payload, size_t offset) {
  memset(payload + offset, 0, sizeof(uint64_t));
  if (heap->verify != GRHEAP_VERIFY_OFF) {
    grheap_shadow_set(heap, (uintptr_t)(payload + offset), 0);
  }
}

bool grheap_weak_spec_fits(const GRHEAP_Type * type, size_t bytes) {
  const GRHEAP_WeakSpec * w = type->weak;
  if (w == NULL) {
    return true;
  }
  size_t offsets[2];
  size_t n;
  if (w->kind == GRHEAP_WEAK_CELL) {
    offsets[0] = w->target_offset;
    n = 1;
  } else if (w->kind == GRHEAP_EPHEMERON) {
    offsets[0] = w->key_offset;
    offsets[1] = w->value_offset;
    n = 2;
    if (w->key_offset == w->value_offset) {
      return false;
    }
  } else {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    if (offsets[i] % 8u != 0 || offsets[i] > bytes ||
        bytes - offsets[i] < sizeof(uint64_t)) {
      return false;
    }
  }
  return true;
}

void grheap_weak_each_slot(const GRHEAP_Type * type, void * payload,
    void (*fn)(void * user, const void * slot), void * user) {
  const GRHEAP_WeakSpec * w = type->weak;
  unsigned char * p = payload;
  if (w->kind == GRHEAP_WEAK_CELL) {
    fn(user, p + w->target_offset);
  } else {
    fn(user, p + w->key_offset);
    fn(user, p + w->value_offset);
  }
}

bool grheap_weak_reserve(GRHEAP_Heap * heap) {
  if (heap->nweak < heap->weak_cap) {
    return true;
  }
  size_t cap = heap->weak_cap == 0 ? 16u : heap->weak_cap * 2u;
  if (cap > SIZE_MAX / sizeof(GRHEAP_WeakEntry)) {
    return false;
  }
  GRHEAP_WeakEntry * grown = heap->allocator->realloc_fn(
      heap->allocator->ctx, heap->weak, cap * sizeof *grown);
  if (grown == NULL) {
    return false;
  }
  heap->weak = grown;
  heap->weak_cap = cap;
  return true;
}

void grheap_weak_register(GRHEAP_Heap * heap, void * payload) {
  heap->weak[heap->nweak].object = payload;
  heap->weak[heap->nweak].pending = false;
  heap->nweak++;
}

/* Whether a key keeps its ephemeron's value alive right now. */
static bool key_holds(const GRHEAP_Heap * heap, const GRHEAP_WeakSpec * spec,
    const unsigned char * payload) {
  GRHEAP_Span * s;
  size_t i;
  switch (classify(heap, spec, payload, spec->key_offset, &s, &i)) {
    case REF_IMMEDIATE:
      return true; /* an integer key never dies */
    case REF_LIVE:
      return grheap_bit_get(s->mark, i);
    default:
      return false;
  }
}

static void fixpoint(GRHEAP_Heap * heap) {
  bool changed;
  do {
    changed = false;
    for (size_t e = 0; e < heap->nweak; e++) {
      unsigned char * payload = heap->weak[e].object;
      const GRHEAP_WeakSpec * spec = grheap_header(payload)->type->weak;
      if (spec->kind != GRHEAP_EPHEMERON || !marked(heap, payload) ||
          !key_holds(heap, spec, payload)) {
        continue;
      }
      GRHEAP_Span * s;
      size_t i;
      if (classify(heap, spec, payload, spec->value_offset, &s, &i) ==
              REF_LIVE &&
          !grheap_bit_get(s->mark, i)) {
        grheap_mark_extra(heap, s, i);
        changed = true;
      }
    }
  } while (changed);
}

static void clear_weak_cell(GRHEAP_Heap * heap, GRHEAP_WeakEntry * entry,
    const GRHEAP_WeakSpec * spec, unsigned char * payload) {
  GRHEAP_Span * s;
  size_t i;
  switch (classify(heap, spec, payload, spec->target_offset, &s, &i)) {
    case REF_LIVE:
      if (grheap_bit_get(s->mark, i)) {
        return;
      }
      break;
    case REF_INVALID:
      heap->stats.invalid_pointers++;
      break;
    default:
      return;
  }
  clear_slot(heap, payload, spec->target_offset);
  entry->pending = true;
  heap->stats.weak_cleared++;
}

static void clear_ephemeron(GRHEAP_Heap * heap, GRHEAP_WeakEntry * entry,
    const GRHEAP_WeakSpec * spec, unsigned char * payload) {
  GRHEAP_Span * ks;
  size_t ki;
  RefState key = classify(heap, spec, payload, spec->key_offset, &ks, &ki);
  bool holds = key == REF_IMMEDIATE || (key == REF_LIVE && grheap_bit_get(ks->mark, ki));
  bool cleared = false;

  if (key == REF_INVALID) {
    heap->stats.invalid_pointers++;
  }
  if (!holds) {
    if (key != REF_NULL) {
      clear_slot(heap, payload, spec->key_offset);
      cleared = true;
    }
  }
  GRHEAP_Span * vs;
  size_t vi;
  RefState value = classify(heap, spec, payload, spec->value_offset, &vs, &vi);
  if (!holds) {
    /* Whatever the value is, nothing keeps it for this entry any more. */
    if (value != REF_NULL) {
      if (value == REF_INVALID) {
        heap->stats.invalid_pointers++;
      }
      clear_slot(heap, payload, spec->value_offset);
      cleared = true;
    }
  } else if (value == REF_INVALID) {
    heap->stats.invalid_pointers++;
    clear_slot(heap, payload, spec->value_offset);
    cleared = true;
  }
  if (cleared) {
    entry->pending = true;
    heap->stats.ephemerons_cleared++;
  }
}

void grheap_weak_process(GRHEAP_Heap * heap) {
  if (heap->nweak == 0) {
    return;
  }
  fixpoint(heap);
  size_t kept = 0;
  for (size_t e = 0; e < heap->nweak; e++) {
    GRHEAP_WeakEntry entry = heap->weak[e];
    if (!marked(heap, entry.object)) {
      continue; /* the cell is dead: dropped, with no callback */
    }
    unsigned char * payload = entry.object;
    const GRHEAP_WeakSpec * spec = grheap_header(payload)->type->weak;
    if (spec->kind == GRHEAP_WEAK_CELL) {
      clear_weak_cell(heap, &entry, spec, payload);
    } else {
      clear_ephemeron(heap, &entry, spec, payload);
    }
    heap->weak[kept++] = entry;
  }
  heap->nweak = kept;
}

void grheap_weak_run_cleared(GRHEAP_Heap * heap) {
  heap->busy++;
  for (size_t e = 0; e < heap->nweak; e++) {
    if (!heap->weak[e].pending) {
      continue;
    }
    heap->weak[e].pending = false;
    void * payload = heap->weak[e].object;
    const GRHEAP_WeakSpec * spec = grheap_header(payload)->type->weak;
    if (spec->cleared != NULL) {
      spec->cleared(payload);
    }
  }
  heap->busy--;
}

void grheap_weak_free(GRHEAP_Heap * heap) {
  if (heap->weak != NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, heap->weak);
  }
  heap->weak = NULL;
  heap->nweak = 0;
  heap->weak_cap = 0;
}
