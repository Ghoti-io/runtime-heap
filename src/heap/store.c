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
 * `gc_store`: the sanctioned pointer stores.
 *
 * In this mode, a stop-the-world non-moving full mark, a store needs no
 * barrier work, so each one writes and counts. They are real functions so that
 * a later mode with a barrier changes this file and no caller (AD-11). With
 * barrier-verify on, each also checks its holder and records the value in the
 * shadow table (verify.c).
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

/* heap_internal.h does not include store.h, and the definitions below take
 * their export attribute from the declarations there; without it they are
 * hidden by -fvisibility=hidden and the shared library lacks them while every
 * test, which links the archive, stays green. */
#include <ghoti.io/runtime-heap/store.h>

#include <string.h>

GRHEAP_Result grheap_store(
    GRHEAP_Heap * heap, void * holder, void ** slot, void * value) {
  if (!grheap_usable(heap) || slot == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  *slot = value;
  heap->stats.stores++;
  /* The holder is for a barrier, which this mode does not have; verify checks
   * it, and records what was written. */
  if (heap->verify != GRHEAP_VERIFY_OFF) {
    grheap_verify_store(heap, holder, slot, 1);
  }
  return GRHEAP_OK;
}

GRHEAP_Result grheap_store_range(GRHEAP_Heap * heap, void * holder,
    void ** dst, void * const * src, size_t count) {
  if (!grheap_usable(heap) || (count != 0 && (dst == NULL || src == NULL)) ||
      count > SIZE_MAX / sizeof(void *)) {
    return GRHEAP_ERR_INVALID;
  }
  if (count != 0) {
    memmove(dst, src, count * sizeof(void *));
  }
  heap->stats.stores += count;
  if (heap->verify != GRHEAP_VERIFY_OFF && count != 0) {
    grheap_verify_store(heap, holder, dst, count);
  }
  return GRHEAP_OK;
}

GRHEAP_Result grheap_store_word(
    GRHEAP_Heap * heap, void * holder, uint64_t * slot, uint64_t value) {
  if (!grheap_usable(heap) || slot == NULL) {
    return GRHEAP_ERR_INVALID;
  }
  *slot = value;
  heap->stats.stores++;
  if (heap->verify != GRHEAP_VERIFY_OFF) {
    grheap_verify_store(heap, holder, slot, 1);
  }
  return GRHEAP_OK;
}
