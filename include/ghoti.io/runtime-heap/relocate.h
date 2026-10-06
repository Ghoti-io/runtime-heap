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
 * @file relocate.h
 * @stability free
 *
 * Relocation torture: a test-only mode that moves objects at every collection.
 *
 * The collector never moves an object, so nothing proves that a reference a
 * precise walk reports is one a collector may rewrite. A compiled frame's
 * stack-map slot, a derived pointer, or an engine field reported through a copy
 * would work today and break the first time a moving collector exists (AD-12,
 * AD-28). This mode moves every unpinned object at every collection, rewrites
 * every reference the collector was shown, and poisons the old cell, so a
 * reference that was visited but not updated reads poison and fails loudly.
 *
 * It exists only in a build made with `RELOCATE=yes`. This header is installed
 * only by such a build, and the functions below are in no other build's library;
 * the one trace of the feature a normal build carries is
 * ::grheap_relocation_available (options.h), which says it is absent. A
 * relocation build is installed into a private prefix under the same package
 * name, so a consumer selects it with `PKG_CONFIG_PATH` and tests the heap it
 * linked with ::grheap_relocation_available.
 *
 * What moves: every marked object that no conservative range hit and no pin
 * holds. A conservative hit or a pin keeps its object in place (AD-17); that is
 * the only exemption. What is rewritten, by the new addresses and nothing else:
 * every precise slot a root source or an object's `trace` function reports,
 * registered root slots and handles, weak entries and the weak and ephemeron
 * slots in payloads, the ID table and its address index, and the
 * barrier-verify shadow table. A moved object's `destroy` hook is not run: the
 * object lives on at its new address.
 *
 * It runs only inside a collection, and never during ::grheap_image_take or a
 * restore. Torture (::grheap_options_set_torture) makes a collection happen at
 * every GC point, which with this mode is a move at every GC point.
 *
 * The run-time switch is the environment variable `GRHEAP_RELOCATE=1` (the
 * default of ::grheap_options_create, as `GRHEAP_TORTURE` is) or
 * ::grheap_options_set_relocate, which wins.
 */

#ifndef GHOTI_IO_GRHEAP_RELOCATE_H
#define GHOTI_IO_GRHEAP_RELOCATE_H

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/core.h>
#include <ghoti.io/runtime-heap/heap.h>
#include <ghoti.io/runtime-heap/options.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the relocation mode has done to a heap. */
typedef struct GRHEAP_RelocationStats {
  uint64_t collections;         ///< Collections that ran the relocation phase.
  uint64_t moved;               ///< Objects moved.
  uint64_t moved_bytes;         ///< Cell bytes moved.
  uint64_t stayed_pinned;       ///< Objects left in place because they were pinned.
  uint64_t stayed_conservative; ///< Objects left in place by a conservative hit.
  uint64_t stayed_no_cell;      ///< Objects left in place because no cell could be had.
  uint64_t skipped;             ///< Collections that moved nothing (no table could be had, or a restore was pending).
} GRHEAP_RelocationStats;

/**
 * @brief Turns relocation on or off for heaps made from these options.
 *
 * @param options The object.
 * @param enabled Whether every collection relocates.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for NULL.
 */
GRHEAP_API GRHEAP_Result grheap_options_set_relocate(
    GRHEAP_Options * options, bool enabled);

/**
 * @brief Whether relocation is on.
 *
 * @param options The object.
 * @return The setting; false for NULL.
 */
GRHEAP_API bool grheap_options_get_relocate(const GRHEAP_Options * options);

/**
 * @brief Reads what the relocation mode has done.
 *
 * @param heap The heap, from its owning thread.
 * @param out_stats Receives the counters. Written only on success.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for a NULL argument or a thread
 *   that does not own the heap.
 */
GRHEAP_API GRHEAP_Result grheap_relocation_stats(
    const GRHEAP_Heap * heap, GRHEAP_RelocationStats * out_stats);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRHEAP_RELOCATE_H */
