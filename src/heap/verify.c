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
 * Barrier-verify: a shadow of every pointer stored through a `grheap_store`,
 * compared with the slots themselves.
 *
 * No mode of this heap has a barrier, so the only fact there is to check is
 * that every write to a slot of a heap object passed through the store
 * functions, which is what a barrier will need. A shadow answers it exactly
 * and portably: the table maps a slot's address to the last value written
 * through a store, and a slot whose value differs was written some other way.
 * An absent entry means the slot must hold zero (an object is zero-filled when
 * it is allocated, and a store of zero removes the entry).
 *
 * What is compared: every slot a live object's `trace` function reports (the
 * tracer is put in VERIFY mode and the slots arrive in
 * `grheap_trace_slot`/`grheap_trace_word` as always), and every weak and
 * ephemeron slot. The walk runs at the start of every collection and in
 * `grheap_verify`. Each store also checks its holder.
 *
 * The table is an open-addressing hash with linear probing and backward-shift
 * deletion, kept in the context's allocator. If it cannot grow, the heap stops
 * trusting it (`shadow_lost`) rather than guess: an absent entry would read as
 * a violation. Nothing the program computes depends on any of this.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the objects ------------------------------------------------------ */

void grheap_each_object(GRHEAP_Heap * heap,
    void (*fn)(void * user, void * payload), void * user) {
  for (size_t i = 0; i < heap->nspans; i++) {
    GRHEAP_Span * s = heap->spans[i];
    if (s->kind == GRHEAP_SPAN_ARENA) {
      size_t offset = 0;
      while (offset < s->bump) {
        void * payload = (void *)(s->base + offset + GRHEAP_HEADER_BYTES);
        fn(user, payload);
        offset += grheap_cell_of_size(grheap_header(payload)->size);
      }
    } else {
      for (size_t c = 0; c < s->bump; c++) {
        if (grheap_bit_get(s->alloc, c)) {
          fn(user, grheap_payload(s, c));
        }
      }
    }
  }
}

bool grheap_object_containing(const GRHEAP_Heap * heap, uintptr_t addr,
    uintptr_t * out_payload, uintptr_t * out_end) {
  GRHEAP_Span * s = grheap_span_find(heap, addr);
  if (s == NULL) {
    return false;
  }
  uintptr_t start;
  uintptr_t end;
  if (s->kind == GRHEAP_SPAN_ARENA) {
    size_t g = (addr - s->base) / 16u;
    if ((addr - s->base) >= s->bump) {
      return false;
    }
    /* The object's cell starts at the nearest granule at or below this one
     * that has its bit set. */
    for (;;) {
      if (grheap_bit_get(s->alloc, g)) {
        break;
      }
      if (g == 0) {
        return false;
      }
      g--;
    }
    start = s->base + g * 16u;
    end = start + grheap_cell_of_size(((GRHEAP_Header *)start)->size);
  } else {
    size_t index = (addr - s->base) / s->cell;
    if (index >= s->ncells || !grheap_bit_get(s->alloc, index)) {
      return false;
    }
    start = s->base + index * s->cell;
    end = start + s->cell;
  }
  if (addr < start + GRHEAP_HEADER_BYTES || addr >= end) {
    return false;
  }
  *out_payload = start + GRHEAP_HEADER_BYTES;
  *out_end = end;
  return true;
}

/* ---- the shadow table ------------------------------------------------- */

static uint64_t mix(uint64_t x) {
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return x;
}

static size_t home(const GRHEAP_Heap * heap, uintptr_t slot) {
  return (size_t)mix((uint64_t)(slot >> 3)) & (heap->shadow_cap - 1u);
}

/* The entry for `slot`, or NULL. */
static GRHEAP_ShadowEntry * shadow_find(const GRHEAP_Heap * heap, uintptr_t slot) {
  if (heap->shadow_cap == 0) {
    return NULL;
  }
  size_t mask = heap->shadow_cap - 1u;
  for (size_t i = home(heap, slot);; i = (i + 1u) & mask) {
    GRHEAP_ShadowEntry * e = &heap->shadow[i];
    if (e->slot == slot) {
      return e;
    }
    if (e->slot == 0) {
      return NULL;
    }
  }
}

static void shadow_remove(GRHEAP_Heap * heap, GRHEAP_ShadowEntry * entry) {
  size_t mask = heap->shadow_cap - 1u;
  size_t i = (size_t)(entry - heap->shadow);
  size_t j = i;
  for (;;) {
    j = (j + 1u) & mask;
    if (heap->shadow[j].slot == 0) {
      break;
    }
    size_t k = home(heap, heap->shadow[j].slot);
    /* Leave an entry whose home lies cyclically in (i, j]: moving it to i
     * would put it before its home. */
    bool stays = i <= j ? (i < k && k <= j) : (i < k || k <= j);
    if (stays) {
      continue;
    }
    heap->shadow[i] = heap->shadow[j];
    i = j;
  }
  heap->shadow[i].slot = 0;
  heap->shadow[i].value = 0;
  heap->shadow_count--;
}

static bool shadow_grow(GRHEAP_Heap * heap) {
  size_t cap = heap->shadow_cap == 0 ? 64u : heap->shadow_cap * 2u;
  if (cap > SIZE_MAX / sizeof(GRHEAP_ShadowEntry)) {
    return false;
  }
  GRHEAP_ShadowEntry * fresh =
      heap->allocator->calloc_fn(heap->allocator->ctx, cap, sizeof *fresh);
  if (fresh == NULL) {
    return false;
  }
  GRHEAP_ShadowEntry * old = heap->shadow;
  size_t old_cap = heap->shadow_cap;
  heap->shadow = fresh;
  heap->shadow_cap = cap;
  for (size_t i = 0; i < old_cap; i++) {
    if (old[i].slot != 0) {
      size_t mask = cap - 1u;
      size_t at = home(heap, old[i].slot);
      while (fresh[at].slot != 0) {
        at = (at + 1u) & mask;
      }
      fresh[at] = old[i];
    }
  }
  if (old != NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, old);
  }
  return true;
}

void grheap_shadow_set(GRHEAP_Heap * heap, uintptr_t slot, uint64_t value) {
  GRHEAP_ShadowEntry * e = shadow_find(heap, slot);
  if (value == 0) {
    if (e != NULL) {
      shadow_remove(heap, e);
    }
    return;
  }
  if (e != NULL) {
    e->value = value;
    return;
  }
  if ((heap->shadow_count + 1u) * 4u > heap->shadow_cap * 3u &&
      !shadow_grow(heap)) {
    heap->shadow_lost = true;
    return;
  }
  size_t mask = heap->shadow_cap - 1u;
  size_t at = home(heap, slot);
  while (heap->shadow[at].slot != 0) {
    at = (at + 1u) & mask;
  }
  heap->shadow[at].slot = slot;
  heap->shadow[at].value = value;
  heap->shadow_count++;
}

void grheap_shadow_forget_range(
    GRHEAP_Heap * heap, uintptr_t start, size_t bytes) {
  if (heap->shadow_count == 0) {
    return;
  }
  uintptr_t first = (start + 7u) & ~(uintptr_t)7u;
  for (uintptr_t a = first; a + 8u <= start + bytes && heap->shadow_count != 0;
      a += 8u) {
    GRHEAP_ShadowEntry * e = shadow_find(heap, a);
    if (e != NULL) {
      shadow_remove(heap, e);
    }
  }
}

void grheap_shadow_clear(GRHEAP_Heap * heap) {
  if (heap->shadow != NULL) {
    memset(heap->shadow, 0, heap->shadow_cap * sizeof *heap->shadow);
  }
  heap->shadow_count = 0;
  heap->shadow_lost = false;
}

void grheap_shadow_free(GRHEAP_Heap * heap) {
  if (heap->shadow != NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, heap->shadow);
  }
  heap->shadow = NULL;
  heap->shadow_cap = 0;
  heap->shadow_count = 0;
}

/* ---- violations ------------------------------------------------------- */

static void violation(GRHEAP_Heap * heap, const GRHEAP_Type * type,
    const void * payload, const char * what, uintptr_t slot, uint64_t expected,
    uint64_t found) {
  heap->stats.verify_violations++;
  if (heap->verify == GRHEAP_VERIFY_ABORT) {
    fprintf(stderr,
        "barrier-verify: %s: object type \"%s\" at %p, slot %p: expected "
        "0x%llx, found 0x%llx\n",
        what, type != NULL && type->name != NULL ? type->name : "(none)",
        payload, (const void *)slot, (unsigned long long)expected,
        (unsigned long long)found);
    fflush(stderr);
    abort();
  }
}

static uint64_t load(const void * slot) {
  uint64_t v;
  memcpy(&v, slot, sizeof v);
  return v;
}

void grheap_verify_slot(GRHEAP_Tracer * tracer, const void * slot) {
  GRHEAP_Heap * heap = tracer->heap;
  uint64_t found = load(slot);
  GRHEAP_ShadowEntry * e = shadow_find(heap, (uintptr_t)slot);
  uint64_t expected = e != NULL ? e->value : 0;
  if (found != expected) {
    tracer->found++;
    violation(heap, tracer->type, tracer->payload,
        "a slot was written without grheap_store", (uintptr_t)slot, expected,
        found);
  }
}

typedef struct VerifyWalk {
  GRHEAP_Heap * heap;
  uint64_t found;
} VerifyWalk;

static void weak_slot(void * user, const void * slot) {
  GRHEAP_Tracer * tracer = user;
  grheap_verify_slot(tracer, slot);
}

static void walk_object(void * user, void * payload) {
  VerifyWalk * w = user;
  const GRHEAP_Type * type = grheap_header(payload)->type;
  GRHEAP_Tracer tracer = {.heap = w->heap,
      .mode = GRHEAP_TRACE_VERIFY,
      .type = type,
      .payload = payload,
      .found = 0};
  if (type->trace != NULL) {
    type->trace(&tracer, payload);
  }
  if (grheap_type_weak(type) != NULL) {
    grheap_weak_each_slot(type, payload, weak_slot, &tracer);
  }
  w->found += tracer.found;
}

uint64_t grheap_verify_walk(GRHEAP_Heap * heap) {
  VerifyWalk w = {heap, 0};
  grheap_each_object(heap, walk_object, &w);
  return w.found;
}

GRHEAP_Result grheap_verify(GRHEAP_Heap * heap, uint64_t * out_violations) {
  if (!grheap_usable(heap) || out_violations == NULL || heap->busy != 0) {
    return GRHEAP_ERR_INVALID;
  }
  if (heap->verify == GRHEAP_VERIFY_OFF) {
    *out_violations = 0;
    return GRHEAP_OK;
  }
  if (heap->shadow_lost) {
    return GRHEAP_ERR_OOM;
  }
  heap->busy++;
  uint64_t found = grheap_verify_walk(heap);
  heap->busy--;
  *out_violations = found;
  return GRHEAP_OK;
}

void grheap_verify_store(GRHEAP_Heap * heap, const void * holder,
    const void * slot, size_t count) {
  uintptr_t first = (uintptr_t)slot;
  uintptr_t last = first + (count == 0 ? 0 : count * 8u - 8u);
  uintptr_t payload;
  uintptr_t end;
  bool in_heap = grheap_object_containing(heap, first, &payload, &end);

  if (holder != NULL) {
    uintptr_t hpayload;
    uintptr_t hend;
    const GRHEAP_Type * htype = NULL;
    if (!grheap_object_containing(heap, (uintptr_t)holder, &hpayload, &hend)) {
      violation(heap, NULL, holder, "the holder is not a heap object", first, 0,
          0);
    } else {
      htype = grheap_header((void *)hpayload)->type;
      if (first < hpayload || last + 8u > hend) {
        violation(heap, htype, (void *)hpayload,
            "the slot is not inside the holder", first, 0, 0);
      }
    }
  } else if (in_heap) {
    violation(heap, grheap_header((void *)payload)->type, (void *)payload,
        "a NULL holder for a slot inside a heap object", first, 0, 0);
  }

  if (!in_heap && holder == NULL) {
    return; /* a root slot, a stack variable: nothing to shadow */
  }
  for (size_t i = 0; i < count; i++) {
    const unsigned char * at = (const unsigned char *)slot + i * 8u;
    if (in_heap && (uintptr_t)at + 8u > end) {
      break;
    }
    grheap_shadow_set(heap, (uintptr_t)at, load(at));
  }
}
