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
 * Collection: when it runs, and what a run does.
 *
 * Stop-the-world and non-moving: mark everything reachable, sweep the rest.
 * It runs on `grheap_collect`, in the allocation path when the bytes allocated
 * since the last collection reach the threshold (and then again, once, if a
 * block or mapping cannot be had), and in the key's ACT poll handler when the
 * poll says the context is over its memory budget and asks to reclaim, so a
 * budget overage gets its collection before the verdict (AD-21).
 *
 * The heap is *busy* for the whole run: an allocation, a handle, a pin or a
 * second collection attempted from a `trace` or `destroy` function is refused
 * with ERR_INVALID rather than corrupting the walk.
 *
 * The order of a run (weak.c, id.c, verify.c, sweep.c):
 *
 *   verify walk -> mark roots -> ephemeron fixpoint -> clear weak slots
 *     -> drop dead IDs -> sweep (forget shadow, destroy, poison)
 *     -> `cleared` callbacks
 */

#include <ghoti.io/runtime-heap/macros.h>

#include "heap_internal.h"

void grheap_collect_now(GRHEAP_Heap * heap) {
  heap->busy++;
  if (heap->verify != GRHEAP_VERIFY_OFF && !heap->shadow_lost) {
    (void)grheap_verify_walk(heap);
  }
  grheap_mark(heap);
  grheap_weak_process(heap);
  grheap_ids_drop_dead(heap);
  grheap_sweep(heap);
  grheap_mark_stack_free(heap);
  heap->busy--;

  heap->stats.collections++;
  heap->since_collect = 0;
  /* A heap that is mostly live must not be collected on every allocation. */
  heap->next_threshold = heap->stats.live_bytes > heap->threshold
      ? heap->stats.live_bytes
      : heap->threshold;

  /* After the sweep, with the heap busy again: the weak cells that lost a
   * target are told. They are live, and the free lists are not touched. */
  grheap_weak_run_cleared(heap);
}

GRHEAP_Result grheap_collect(GRHEAP_Heap * heap) {
  if (!grheap_usable(heap) || heap->busy != 0) {
    return GRHEAP_ERR_INVALID;
  }
  if (heap->mode == GRHEAP_MODE_ARENA) {
    return GRHEAP_OK;
  }
  grheap_collect_now(heap);
  return GRHEAP_OK;
}
