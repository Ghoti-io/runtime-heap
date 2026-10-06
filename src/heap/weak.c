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

#include <stdlib.h>
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
  const GRHEAP_WeakSpec * w = grheap_type_weak(type);
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
  const GRHEAP_WeakSpec * w = grheap_type_weak(type);
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

/* The fixpoint by rescanning: each pass looks at every registered entry, and
 * it passes until one marks nothing. Correct always, but a chain registered in
 * the opposite order to its links resolves one link per pass, which is
 * quadratic in the chain. It is what runs when the indexed version below
 * cannot get its memory. */
static void fixpoint_scan(GRHEAP_Heap * heap) {
  bool changed;
  do {
    changed = false;
    for (size_t e = 0; e < heap->nweak; e++) {
      unsigned char * payload = heap->weak[e].object;
      const GRHEAP_WeakSpec * spec = grheap_type_weak(grheap_header(payload)->type);
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

/* The fixpoint by what marking reaches. An ephemeron's value is marked when
 * its own cell is marked and its key holds, so an ephemeron becomes ready at
 * exactly two events: its key being marked, or its cell being marked. Two
 * tables sorted by address say which ephemerons a newly marked cell is the
 * key of, and which it is; the marker reports every cell it newly marks
 * through `mark_hook`, which only queues what has become ready. Each
 * ephemeron is queued once, so the work is the ephemerons times log, plus the
 * log of the ephemerons for each cell the fixpoint itself marks. */
typedef struct {
  uintptr_t address;
  uint32_t entry;
} Link;

typedef struct {
  GRHEAP_Heap * heap;
  Link * keys;         ///< Ephemerons with a live key, by the key's address.
  size_t nkeys;
  Link * owners;       ///< Every ephemeron, by its own address.
  size_t nowners;
  uint32_t * queue;    ///< Ready entries, each at most once.
  size_t head;
  size_t tail;
  unsigned char * queued; ///< Per registry entry.
} Fix;

static int link_order(const void * a, const void * b) {
  const Link * x = a;
  const Link * y = b;
  if (x->address != y->address) {
    return x->address < y->address ? -1 : 1;
  }
  return (x->entry > y->entry) - (x->entry < y->entry);
}

static size_t first_at(const Link * links, size_t n, uintptr_t address) {
  size_t lo = 0;
  size_t hi = n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (links[mid].address < address) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/* Queues entry `e` if it is ready now. */
static void consider(Fix * f, uint32_t e) {
  if (f->queued[e]) {
    return;
  }
  unsigned char * payload = f->heap->weak[e].object;
  const GRHEAP_WeakSpec * spec = grheap_type_weak(grheap_header(payload)->type);
  if (marked(f->heap, payload) && key_holds(f->heap, spec, payload)) {
    f->queued[e] = 1;
    f->queue[f->tail++] = e;
  }
}

static void fix_on_mark(void * user, void * payload) {
  Fix * f = user;
  uintptr_t address = (uintptr_t)payload;
  for (size_t i = first_at(f->keys, f->nkeys, address);
      i < f->nkeys && f->keys[i].address == address; i++) {
    consider(f, f->keys[i].entry);
  }
  size_t o = first_at(f->owners, f->nowners, address);
  if (o < f->nowners && f->owners[o].address == address) {
    consider(f, f->owners[o].entry);
  }
}

/* False, with nothing marked and the hook off, if the tables cannot be had. */
static bool fixpoint_indexed(GRHEAP_Heap * heap) {
  size_t count = 0;
  if (heap->nweak > UINT32_MAX) {
    return false;
  }
  for (size_t e = 0; e < heap->nweak; e++) {
    const GRHEAP_WeakSpec * spec =
        grheap_type_weak(grheap_header(heap->weak[e].object)->type);
    count += spec->kind == GRHEAP_EPHEMERON;
  }
  if (count == 0) {
    return true;
  }
  if (count > SIZE_MAX / (2 * sizeof(Link) + sizeof(uint32_t) + 2)) {
    return false;
  }
  size_t bytes = count * 2 * sizeof(Link) + count * sizeof(uint32_t) + heap->nweak;
  unsigned char * block = heap->allocator->malloc_fn(heap->allocator->ctx, bytes);
  if (block == NULL) {
    return false;
  }
  Fix f = {.heap = heap};
  /* One block, carved by byte offsets so that each part is only ever read as
   * its own type. Link is 16 bytes and 8-aligned, so the arrays that follow it
   * are aligned too. */
  f.keys = (Link *)(void *)block;
  f.owners = (Link *)(void *)(block + count * sizeof(Link));
  f.queue = (uint32_t *)(void *)(block + 2 * count * sizeof(Link));
  f.queued = block + 2 * count * sizeof(Link) + count * sizeof(uint32_t);
  memset(f.queued, 0, heap->nweak);
  for (size_t e = 0; e < heap->nweak; e++) {
    unsigned char * payload = heap->weak[e].object;
    const GRHEAP_WeakSpec * spec = grheap_type_weak(grheap_header(payload)->type);
    if (spec->kind != GRHEAP_EPHEMERON) {
      continue;
    }
    f.owners[f.nowners].address = (uintptr_t)payload;
    f.owners[f.nowners++].entry = (uint32_t)e;
    GRHEAP_Span * s;
    size_t i;
    if (classify(heap, spec, payload, spec->key_offset, &s, &i) == REF_LIVE) {
      f.keys[f.nkeys].address = (uintptr_t)grheap_payload(s, i);
      f.keys[f.nkeys++].entry = (uint32_t)e;
    }
  }
  qsort(f.keys, f.nkeys, sizeof(Link), link_order);
  qsort(f.owners, f.nowners, sizeof(Link), link_order);

  heap->mark_hook = fix_on_mark;
  heap->mark_hook_user = &f;
  for (size_t k = 0; k < f.nowners; k++) {
    consider(&f, f.owners[k].entry);
  }
  while (f.head < f.tail) {
    uint32_t e = f.queue[f.head++];
    unsigned char * payload = heap->weak[e].object;
    const GRHEAP_WeakSpec * spec = grheap_type_weak(grheap_header(payload)->type);
    GRHEAP_Span * s;
    size_t i;
    if (classify(heap, spec, payload, spec->value_offset, &s, &i) == REF_LIVE &&
        !grheap_bit_get(s->mark, i)) {
      grheap_mark_extra(heap, s, i);
    }
  }
  heap->mark_hook = NULL;
  heap->mark_hook_user = NULL;
  heap->allocator->free_fn(heap->allocator->ctx, block);
  return true;
}

static void fixpoint(GRHEAP_Heap * heap) {
  if (!fixpoint_indexed(heap)) {
    fixpoint_scan(heap);
  }
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
    const GRHEAP_WeakSpec * spec = grheap_type_weak(grheap_header(payload)->type);
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
    const GRHEAP_WeakSpec * spec = grheap_type_weak(grheap_header(payload)->type);
    if (spec->cleared != NULL) {
      spec->cleared(payload);
    }
  }
  heap->busy--;
}

#ifdef GRHEAP_RELOCATE
/* A moving collection: every registered cell is at its new address. (The slots
 * inside the payloads are rewritten with the other precise slots.) */
void grheap_weak_relocate(GRHEAP_Heap * heap) {
#ifndef GRHEAP_PLANT_RELOC_WEAK_STALE
  for (size_t e = 0; e < heap->nweak; e++) {
    heap->weak[e].object = grheap_reloc_forward(heap, heap->weak[e].object);
  }
#else
  (void)heap;
#endif
}
#endif

void grheap_weak_free(GRHEAP_Heap * heap) {
  if (heap->weak != NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, heap->weak);
  }
  heap->weak = NULL;
  heap->nweak = 0;
  heap->weak_cap = 0;
}
