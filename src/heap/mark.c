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
 * Marking: from the roots to every reachable object, without recursion.
 *
 * Roots, in order: the registered root slots, the handles, the pinned
 * objects, then whatever the context's root sources report (precise slots,
 * decoded with the value codec, and conservative ranges). Marking uses an
 * explicit stack of payloads. The stack grows through the context's allocator,
 * and a collection must never fail, so when it cannot grow the push is dropped
 * and an overflow flag is set: the object stays marked, and after the stack
 * has drained every marked object is traced again until a whole pass finishes
 * with no overflow. Each overflow means an object was newly marked, so the
 * passes terminate; they are slow only when the allocator is failing.
 *
 * Precise slots are never guessed at. A word that decodes to anything other
 * than the exact start of a live payload is counted and skipped. Only a
 * conservative range accepts an interior pointer.
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
/* A conservative scan reads whatever words a range covers, and a range over a
 * stack covers memory ASan has poisoned (redzones, dead frames). That is the
 * point of the scan, not an error in it. */
#define GRHEAP_NO_ASAN __attribute__((no_sanitize("address")))
#else
#define GRHEAP_NO_ASAN
#endif

bool grheap_decode(const GRHEAP_Heap * heap, uint64_t word, uint64_t * out) {
  const GRHEAP_ValueCodec * c = &heap->codec;
  if ((word & c->tag_mask) != c->tag_value) {
    return false;
  }
  *out = ((word & c->mask) >> c->shift) + c->base;
  return true;
}

static void push(GRHEAP_Heap * heap, void * payload) {
  if (heap->mark_top == heap->mark_cap) {
    size_t cap = heap->mark_cap == 0 ? 256u : heap->mark_cap * 2u;
    void ** grown = NULL;
    if (cap > heap->mark_cap && cap <= SIZE_MAX / sizeof(void *)) {
      grown = heap->allocator->realloc_fn(
          heap->allocator->ctx, heap->mark_stack, cap * sizeof(void *));
    }
    if (grown == NULL) {
      heap->mark_overflow = true;
      return;
    }
    heap->mark_stack = grown;
    heap->mark_cap = cap;
  }
  heap->mark_stack[heap->mark_top++] = payload;
}

/* Marks one allocated cell and queues it for tracing, if it has anything to
 * trace. An already marked cell is left alone: that is what ends cycles. */
static void mark_cell(GRHEAP_Heap * heap, GRHEAP_Span * span, size_t index) {
  if (grheap_bit_get(span->mark, index)) {
    return;
  }
  grheap_bit_set(span->mark, index);
  void * payload = grheap_payload(span, index);
  if (grheap_header(payload)->type->trace != NULL) {
    push(heap, payload);
  }
  if (heap->mark_hook != NULL) {
    heap->mark_hook(heap->mark_hook_user, payload);
  }
}

/* A decoded address that must be exactly a live payload start. */
static void mark_precise(GRHEAP_Heap * heap, uint64_t address) {
  if (address == 0) {
    return;
  }
  GRHEAP_Span * span;
  size_t index;
  if ((uint64_t)(uintptr_t)address != address ||
      !grheap_locate(heap, (uintptr_t)address, true, &span, &index)) {
    heap->stats.invalid_pointers++;
    return;
  }
  mark_cell(heap, span, index);
}

static void trace_payload(GRHEAP_Heap * heap, void * payload) {
  const GRHEAP_Type * type = grheap_header(payload)->type;
  if (type->trace != NULL) {
    GRHEAP_Tracer tracer = {.heap = heap, .mode = GRHEAP_TRACE_MARK};
    type->trace(&tracer, payload);
  }
}

static void drain(GRHEAP_Heap * heap) {
  while (heap->mark_top > 0) {
    trace_payload(heap, heap->mark_stack[--heap->mark_top]);
  }
}

/* Traces every marked object again. What it newly marks is pushed, or sets the
 * overflow flag again if the stack still cannot grow. */
static void rescan(GRHEAP_Heap * heap) {
  for (size_t i = 0; i < heap->nspans; i++) {
    GRHEAP_Span * s = heap->spans[i];
    size_t words = grheap_bit_words(s->bump);
    for (size_t w = 0; w < words; w++) {
      uint64_t bits = s->alloc[w] & s->mark[w];
      for (unsigned b = 0; bits != 0; b++, bits >>= 1) {
        if (bits & 1u) {
          trace_payload(heap, grheap_payload(s, w * 64u + b));
        }
      }
    }
  }
}

static void finish(GRHEAP_Heap * heap) {
  drain(heap);
  while (heap->mark_overflow) {
    heap->mark_overflow = false;
    rescan(heap);
    drain(heap);
  }
}

void grheap_mark_extra(GRHEAP_Heap * heap, GRHEAP_Span * span, size_t index) {
  mark_cell(heap, span, index);
  finish(heap);
}

/* FORGET mode: drops the shadow of a slot a dead object reports. A slot inside
 * the object's own cell was dropped with the cell's range already, so only the
 * slots outside it cost a lookup (a big store would otherwise be forgotten
 * twice). `payload` and `size` name the cell's payload. */
static void forget_slot(const GRHEAP_Tracer * tracer, const void * slot) {
  uintptr_t at = (uintptr_t)slot;
  uintptr_t first = (uintptr_t)tracer->payload;
  if (at >= first && at - first < tracer->size) {
    return;
  }
  grheap_shadow_set(tracer->heap, at, 0); /* zero removes it */
}

void grheap_trace_slot(GRHEAP_Tracer * tracer, void ** slot) {
  if (tracer == NULL || slot == NULL) {
    return;
  }
  if (tracer->mode == GRHEAP_TRACE_VERIFY) {
    grheap_verify_slot(tracer, slot);
    return;
  }
  if (tracer->mode == GRHEAP_TRACE_IMAGE) {
    grheap_image_trace(tracer, slot, false);
    return;
  }
  if (tracer->mode == GRHEAP_TRACE_SHADOW) {
    grheap_shadow_set(tracer->heap, (uintptr_t)slot, (uint64_t)(uintptr_t)*slot);
    return;
  }
  if (tracer->mode == GRHEAP_TRACE_FORGET) {
    forget_slot(tracer, slot);
    return;
  }
  if (tracer->mode == GRHEAP_TRACE_RETAIN) {
    grheap_retention_trace(tracer, slot, false);
    return;
  }
#ifdef GRHEAP_RELOCATE
  if (tracer->mode == GRHEAP_TRACE_RELOCATE) {
    grheap_reloc_slot(tracer, slot, false);
    return;
  }
#endif
  mark_precise(tracer->heap, (uint64_t)(uintptr_t)*slot);
}

void grheap_trace_word(GRHEAP_Tracer * tracer, uint64_t * slot) {
  if (tracer == NULL || slot == NULL) {
    return;
  }
  if (tracer->mode == GRHEAP_TRACE_VERIFY) {
    grheap_verify_slot(tracer, slot);
    return;
  }
  if (tracer->mode == GRHEAP_TRACE_IMAGE) {
    grheap_image_trace(tracer, slot, true);
    return;
  }
  if (tracer->mode == GRHEAP_TRACE_SHADOW) {
    grheap_shadow_set(tracer->heap, (uintptr_t)slot, *slot);
    return;
  }
  if (tracer->mode == GRHEAP_TRACE_FORGET) {
    forget_slot(tracer, slot);
    return;
  }
  if (tracer->mode == GRHEAP_TRACE_RETAIN) {
    grheap_retention_trace(tracer, slot, true);
    return;
  }
#ifdef GRHEAP_RELOCATE
  if (tracer->mode == GRHEAP_TRACE_RELOCATE) {
    grheap_reloc_slot(tracer, slot, true);
    return;
  }
#endif
  uint64_t address;
  if (grheap_decode(tracer->heap, *slot, &address)) {
    mark_precise(tracer->heap, address);
  }
}

static void visit_slot(void * user, uint64_t * slot) {
  GRHEAP_Heap * heap = user;
  uint64_t address;
  if (grheap_decode(heap, *slot, &address)) {
    mark_precise(heap, address);
  }
}

/* A conservative word: anything inside an allocated cell counts, interior and
 * one-past-the-end included. In a non-moving heap marking is all the pinning
 * there is to do; a relocation build also records that the cell must stay. */
static void mark_conservative(GRHEAP_Heap * heap, uint64_t address) {
  GRHEAP_Span * span;
  size_t index;
  if ((uint64_t)(uintptr_t)address != address ||
      !grheap_locate(heap, (uintptr_t)address, false, &span, &index)) {
    return;
  }
  heap->stats.conservative_hits++;
#ifdef GRHEAP_RELOCATE
  grheap_reloc_stay(span, index);
#endif
  mark_cell(heap, span, index);
}

GRHEAP_NO_ASAN static void visit_range(
    void * user, const GRCORE_ConservativeRange * range) {
  GRHEAP_Heap * heap = user;
  if (range->shift >= 64u || range->hi <= range->lo) {
    return;
  }
  /* Whole, aligned words only. The count is taken first so that a range ending
   * near the top of the address space cannot wrap the loop. */
  uint64_t lo = (range->lo + 7u) & ~UINT64_C(7);
  if (lo < range->lo || lo >= range->hi) {
    return;
  }
  uint64_t count = (range->hi - lo) / 8u;
  for (uint64_t i = 0; i < count; i++) {
    uint64_t word;
    __builtin_memcpy(&word, (const void *)(uintptr_t)(lo + i * 8u), sizeof word);
    mark_conservative(heap, ((word & range->mask) >> range->shift) + range->base);
  }
}

void grheap_mark_pinned(GRHEAP_Heap * heap) {
  for (size_t i = 0; i < heap->nspans; i++) {
    GRHEAP_Span * s = heap->spans[i];
    if (s->kind != GRHEAP_SPAN_BLOCK || s->npinned == 0) {
      continue;
    }
    for (size_t c = 0; c < s->ncells; c++) {
      if (s->pins[c] != 0) {
        mark_cell(heap, s, c);
      }
    }
  }
}

void grheap_mark(GRHEAP_Heap * heap) {
  heap->mark_top = 0;
  heap->mark_overflow = false;
#ifdef GRHEAP_RELOCATE
  if (heap->relocate) {
    for (size_t i = 0; i < heap->nspans; i++) {
      GRHEAP_Span * s = heap->spans[i];
      if (s->stay != NULL) {
        memset(s->stay, 0, grheap_bit_words(s->ncells) * sizeof(uint64_t));
      }
    }
  }
#endif

  for (size_t i = 0; i < heap->nroots; i++) {
    mark_precise(heap, (uint64_t)(uintptr_t)*heap->roots[i]);
  }
  finish(heap);

  for (struct GRHEAP_Handle * h = heap->handles; h != NULL; h = h->next) {
    mark_precise(heap, (uint64_t)(uintptr_t)h->object);
  }
  finish(heap);

  grheap_mark_pinned(heap);
  finish(heap);

  GRCORE_RootVisitor visitor = {heap, visit_slot, visit_range};
  grcore_context_enumerate_roots(heap->context, &visitor);
  finish(heap);
}

void grheap_mark_stack_free(GRHEAP_Heap * heap) {
  if (heap->mark_stack != NULL) {
    heap->allocator->free_fn(heap->allocator->ctx, heap->mark_stack);
  }
  heap->mark_stack = NULL;
  heap->mark_top = 0;
  heap->mark_cap = 0;
}
