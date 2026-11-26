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
 * Mark-step torture: an incremental mark that advances by one object at every
 * GC point, with a store barrier, checked against a stop-the-world mark when
 * the cycle ends. The collector itself stays stop-the-world; this is an
 * instrument that makes a store wrong only under an interleaved mark visible
 * today (see options.h, ::GRHEAP_MarkTorture).
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct GRHEAP_Incremental {
  GRHEAP_MarkTorture mode;
  bool active;       /* a cycle is in progress */
  bool snapshot;     /* this cycle uses the deletion barrier */
  bool lost;         /* an allocation failed: the cycle is abandoned */
  uint64_t cycle;    /* cycles started */
  uintptr_t * marked; /* open addressing; 0 is empty */
  size_t marked_cap, nmarked;
  uintptr_t * grey;
  size_t ngrey, grey_cap;
};

static size_t slot_of(uintptr_t key, size_t cap) {
  uint64_t h = (uint64_t)key * UINT64_C(0x9E3779B97F4A7C15);
  return (size_t)(h >> 20) & (cap - 1u);
}

static bool is_marked(const struct GRHEAP_Incremental * m, uintptr_t key) {
  if (m->marked_cap == 0) {
    return false;
  }
  size_t at = slot_of(key, m->marked_cap);
  while (m->marked[at] != 0) {
    if (m->marked[at] == key) {
      return true;
    }
    at = (at + 1u) & (m->marked_cap - 1u);
  }
  return false;
}

static void * grow(GRHEAP_Heap * heap, void * p, size_t bytes) {
  return heap->allocator->realloc_fn(heap->allocator->ctx, p, bytes);
}

/* Records `key` as marked. True when it was new. */
static bool mark_key(GRHEAP_Heap * heap, uintptr_t key) {
  struct GRHEAP_Incremental * m = heap->inc;
  if (is_marked(m, key)) {
    return false;
  }
  if ((m->nmarked + 1u) * 2u > m->marked_cap) {
    size_t cap = m->marked_cap == 0 ? 64u : m->marked_cap * 2u;
    uintptr_t * fresh = heap->allocator->calloc_fn(
        heap->allocator->ctx, cap, sizeof *fresh);
    if (fresh == NULL) {
      m->lost = true;
      return false;
    }
    for (size_t i = 0; i < m->marked_cap; i++) {
      if (m->marked[i] != 0) {
        size_t at = slot_of(m->marked[i], cap);
        while (fresh[at] != 0) {
          at = (at + 1u) & (cap - 1u);
        }
        fresh[at] = m->marked[i];
      }
    }
    heap->allocator->free_fn(heap->allocator->ctx, m->marked);
    m->marked = fresh;
    m->marked_cap = cap;
  }
  size_t at = slot_of(key, m->marked_cap);
  while (m->marked[at] != 0) {
    at = (at + 1u) & (m->marked_cap - 1u);
  }
  m->marked[at] = key;
  m->nmarked++;
  return true;
}

/* Shades an object grey: marked, and queued to be traced. */
static void shade(GRHEAP_Heap * heap, void * payload) {
  struct GRHEAP_Incremental * m = heap->inc;
  if (!mark_key(heap, (uintptr_t)payload)) {
    return;
  }
  if (m->ngrey == m->grey_cap) {
    size_t cap = m->grey_cap == 0 ? 64u : m->grey_cap * 2u;
    uintptr_t * grown = grow(heap, m->grey, cap * sizeof *grown);
    if (grown == NULL) {
      m->lost = true;
      return;
    }
    m->grey = grown;
    m->grey_cap = cap;
  }
  m->grey[m->ngrey++] = (uintptr_t)payload;
}

/* Shades what a word names, if it names the start of a live payload. */
static void shade_address(GRHEAP_Heap * heap, uint64_t address) {
  GRHEAP_Span * span;
  size_t index;
  if (address != 0 && (uint64_t)(uintptr_t)address == address &&
      grheap_locate(heap, (uintptr_t)address, true, &span, &index)) {
    shade(heap, grheap_payload(span, index));
  }
}

void grheap_inc_visit(GRHEAP_Tracer * tracer, const void * slot, bool encoded) {
  /* A barrier shades only the slots it was given; a mark step all of them. */
  if (tracer->base != NULL &&
      ((uintptr_t)slot < (uintptr_t)tracer->base ||
          (uintptr_t)slot - (uintptr_t)tracer->base >= tracer->size)) {
    return;
  }
  uint64_t word;
  memcpy(&word, slot, sizeof word);
  uint64_t address = word;
  if (encoded && !grheap_decode_with(tracer->codec, word, &address)) {
    return;
  }
  shade_address(tracer->heap, address);
}

static void trace_object(GRHEAP_Heap * heap, void * payload,
    const void * only, size_t only_bytes) {
  const GRHEAP_Type * type = grheap_header(payload)->type;
  if (type->trace == NULL) {
    return;
  }
  GRHEAP_Tracer tracer = {.heap = heap, .mode = GRHEAP_TRACE_STEP,
      .codec = grheap_type_codec(heap, type),
      .base = (unsigned char *)(uintptr_t)only, .size = only_bytes};
  heap->busy++;
  type->trace(&tracer, payload);
  heap->busy--;
}

static void root_slot(void * user, uint64_t * slot) {
  GRHEAP_Heap * heap = user;
  uint64_t address;
  if (grheap_decode_with(grheap_root_codec(heap), *slot, &address)) {
    shade_address(heap, address);
  }
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((no_sanitize("address")))
#endif
static void root_range(void * user, const GRCORE_ConservativeRange * range) {
  GRHEAP_Heap * heap = user;
  if (range->shift >= 64u || range->hi <= range->lo) {
    return;
  }
  uint64_t lo = (range->lo + 7u) & ~UINT64_C(7);
  if (lo < range->lo || lo >= range->hi) {
    return;
  }
  for (uint64_t i = 0, n = (range->hi - lo) / 8u; i < n; i++) {
    uint64_t word;
    __builtin_memcpy(&word, (const void *)(uintptr_t)(lo + i * 8u), sizeof word);
    uint64_t address = ((word & range->mask) >> range->shift) + range->base;
    GRHEAP_Span * span;
    size_t index;
    if ((uint64_t)(uintptr_t)address == address &&
        grheap_locate(heap, (uintptr_t)address, false, &span, &index)) {
      shade(heap, grheap_payload(span, index));
    }
  }
}

static void shade_roots(GRHEAP_Heap * heap) {
  for (size_t i = 0; i < heap->nroots; i++) {
    shade_address(heap, (uint64_t)(uintptr_t)*heap->roots[i]);
  }
  for (struct GRHEAP_Handle * h = heap->handles; h != NULL; h = h->next) {
    shade_address(heap, (uint64_t)(uintptr_t)h->object);
  }
  for (size_t i = 0; i < heap->nspans; i++) {
    GRHEAP_Span * s = heap->spans[i];
    if (s->kind != GRHEAP_SPAN_BLOCK || s->npinned == 0) {
      continue;
    }
    for (size_t c = 0; c < s->ncells; c++) {
      if (s->pins[c] != 0) {
        shade(heap, grheap_payload(s, c));
      }
    }
  }
  GRCORE_RootVisitor visitor = {heap, root_slot, root_range};
  (void)grheap_roots_enumerate(heap, &visitor);
}

static void drain(GRHEAP_Heap * heap) {
  struct GRHEAP_Incremental * m = heap->inc;
  while (m->ngrey > 0 && !m->lost) {
    trace_object(heap, (void *)m->grey[--m->ngrey], NULL, 0);
  }
}

/* The ephemerons: a live one whose key is marked (or is an immediate) holds
 * its value, as the collector's fixpoint does. */
static void drain_ephemerons(GRHEAP_Heap * heap) {
  struct GRHEAP_Incremental * m = heap->inc;
  for (bool changed = true; changed && !m->lost;) {
    changed = false;
    for (size_t e = 0; e < heap->nweak && !m->lost; e++) {
      unsigned char * payload = heap->weak[e].object;
      const GRHEAP_Type * type = grheap_header(payload)->type;
      const GRHEAP_WeakSpec * spec = grheap_type_weak(type);
      if (spec->kind != GRHEAP_EPHEMERON || !is_marked(m, (uintptr_t)payload)) {
        continue;
      }
      uint64_t key, value;
      memcpy(&key, payload + spec->key_offset, sizeof key);
      memcpy(&value, payload + spec->value_offset, sizeof value);
      if (spec->encoded) {
        const GRHEAP_ValueCodec * c = grheap_type_codec(heap, type);
        uint64_t k, v;
        bool key_ref = grheap_decode_with(c, key, &k);
        if (key_ref && (k == 0 || !is_marked(m, (uintptr_t)k))) {
          continue;
        }
        if (!grheap_decode_with(c, value, &v)) {
          continue;
        }
        value = v;
      } else if (key == 0 || !is_marked(m, (uintptr_t)key)) {
        continue;
      }
      size_t before = m->nmarked;
      shade_address(heap, value);
      if (m->nmarked != before) {
        drain(heap);
        changed = true;
      }
    }
  }
}

static void reset(GRHEAP_Heap * heap) {
  struct GRHEAP_Incremental * m = heap->inc;
  m->active = false;
  m->lost = false;
  m->ngrey = 0;
  m->nmarked = 0;
  if (m->marked_cap != 0) {
    memset(m->marked, 0, m->marked_cap * sizeof *m->marked);
  }
}

/* The cycle's end: finish the marking, then ask a stop-the-world mark what is
 * reachable. Anything it finds that the interleaved mark did not is a store
 * the barrier did not see. */
static void terminate(GRHEAP_Heap * heap) {
  struct GRHEAP_Incremental * m = heap->inc;
  if (!m->snapshot) {
    shade_roots(heap); /* root writes are not barriered: scan them again */
    drain(heap);
  }
  drain_ephemerons(heap);
  if (m->lost) {
    reset(heap);
    return;
  }
  heap->busy++;
  grheap_mark(heap);
  heap->busy--;
  for (size_t i = 0; i < heap->nspans; i++) {
    GRHEAP_Span * s = heap->spans[i];
    if (s->kind != GRHEAP_SPAN_BLOCK) {
      continue;
    }
    for (size_t c = 0; c < s->bump; c++) {
      if (!grheap_bit_get(s->alloc, c) || !grheap_bit_get(s->mark, c)) {
        continue;
      }
      void * payload = grheap_payload(s, c);
      if (!is_marked(m, (uintptr_t)payload)) {
        const GRHEAP_Type * type = grheap_header(payload)->type;
        heap->stats.mark_violations++;
        if (m->mode == GRHEAP_MARK_TORTURE_ABORT) {
          fprintf(stderr,
              "mark-torture: a reachable object was not marked by the %s "
              "barrier: object type \"%s\" at %p\n",
              m->snapshot ? "deletion" : "insertion",
              type->name != NULL ? type->name : "?", payload);
          abort();
        }
      }
    }
  }
  /* Put the mark bits back as a collection expects to find them, then collect
   * for real, which also poisons what the cycle freed. */
  for (size_t i = 0; i < heap->nspans; i++) {
    GRHEAP_Span * s = heap->spans[i];
    if (s->kind == GRHEAP_SPAN_BLOCK) {
      memset(s->mark, 0, grheap_bit_words(s->ncells) * sizeof(uint64_t));
    }
  }
  grheap_mark_stack_free(heap);
  heap->stats.mark_cycles++;
  reset(heap);
  grheap_collect_now(heap);
}

void grheap_inc_step(GRHEAP_Heap * heap) {
  struct GRHEAP_Incremental * m = heap->inc;
  if (m == NULL || heap->mode != GRHEAP_MODE_COLLECTED || heap->busy != 0) {
    return;
  }
  heap->stats.mark_steps++;
  if (!m->active) {
    reset(heap);
    m->snapshot = (m->cycle & 1u) != 0; /* the disciplines alternate */
    m->cycle++;
    m->active = true;
    shade_roots(heap);
    return;
  }
  if (m->ngrey > 0) {
    trace_object(heap, (void *)m->grey[--m->ngrey], NULL, 0);
    return;
  }
  terminate(heap);
}

void grheap_inc_collected(GRHEAP_Heap * heap) {
  if (heap->inc != NULL && heap->inc->active) {
    reset(heap); /* objects may be freed or moved: the cycle starts again */
  }
}

void grheap_inc_allocated(GRHEAP_Heap * heap, void * payload) {
  if (heap->inc != NULL && heap->inc->active) {
    /* Allocated black: marked, and never queued. */
    (void)mark_key(heap, (uintptr_t)payload);
  }
}

/* The store barrier, called on both sides of a write: before it (the deletion
 * discipline shades what the slots held) and after it (the insertion discipline
 * shades what they now hold). The values are found by the holder's own trace,
 * which knows each slot's codec, and also read as raw pointers and as the
 * holder's codec, so a slot a trace does not report yet is not missed. Extra
 * shading only makes the check less strict, never wrong. */
void grheap_inc_barrier(GRHEAP_Heap * heap, const void * holder,
    const void * slots, size_t count, bool before) {
  struct GRHEAP_Incremental * m = heap->inc;
  if (m == NULL || !m->active || before != m->snapshot || count == 0) {
    return;
  }
  GRHEAP_Span * span;
  size_t index;
  void * h = (void *)(uintptr_t)holder;
  const GRHEAP_Type * type = NULL;
  if (h != NULL && grheap_locate(heap, (uintptr_t)h, true, &span, &index)) {
    type = grheap_header(h)->type;
    trace_object(heap, h, slots, count * sizeof(uint64_t));
  }
  for (size_t i = 0; i < count; i++) {
    uint64_t word;
    memcpy(&word, (const unsigned char *)slots + i * sizeof word, sizeof word);
    shade_address(heap, word);
    uint64_t address;
    if (type != NULL && grheap_decode_with(grheap_type_codec(heap, type), word, &address)) {
      shade_address(heap, address);
    }
  }
}

bool grheap_inc_create(GRHEAP_Heap * heap, GRHEAP_MarkTorture mode) {
  heap->inc = heap->allocator->calloc_fn(heap->allocator->ctx, 1, sizeof *heap->inc);
  if (heap->inc == NULL) {
    return false;
  }
  heap->inc->mode = mode;
  return true;
}

void grheap_inc_free(GRHEAP_Heap * heap) {
  struct GRHEAP_Incremental * m = heap->inc;
  if (m == NULL) {
    return;
  }
  heap->allocator->free_fn(heap->allocator->ctx, m->marked);
  heap->allocator->free_fn(heap->allocator->ctx, m->grey);
  heap->allocator->free_fn(heap->allocator->ctx, m);
  heap->inc = NULL;
}
