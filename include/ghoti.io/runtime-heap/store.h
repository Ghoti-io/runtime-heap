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
 * @file store.h
 * @stability stable
 *
 * `gc_store`: the only sanctioned way to write a pointer into a heap object,
 * and the barrier contract it stands for.
 *
 * Every pointer store goes through these functions (AD-11): hand-written C, an
 * interpreter's write to an object field, and out-of-line storage that a
 * `trace` function reports all included. In this heap mode, a stop-the-world
 * non-moving full mark, there is no barrier work to do, so each call stores
 * and counts. They are exported functions rather than inline macros so that a
 * later generational or incremental mode can add a barrier without a single
 * caller changing. The contract, in its unshared (implemented) and shared
 * (designed) variants, is in `documentation/design.md`.
 *
 * `holder` is the heap object whose payload contains the slot; the barrier of
 * a later mode needs to know it. It may be NULL for a store into a slot that
 * is not inside any heap object: a registered root slot, a stack variable, a
 * C structure. A store into a heap object with a NULL holder is a bug that a
 * barrier mode would miss, and barrier-verify (::GRHEAP_VERIFY_COUNT) counts it
 * as a violation. So is a holder whose cell does not contain the slot.
 * Either way the store still happens and the call still returns ::GRHEAP_OK.
 *
 * Barrier-verify polices the discipline a barrier will rely on: it records the
 * last value written through these functions to each slot of a heap object, and
 * ::grheap_verify (and every collection) compares that with the slot, so a
 * pointer written directly is found. This mode has no barrier work to do; the
 * verify mode is the only thing the holder is used for.
 *
 * Threads: owner only, as everything in the heap.
 */

#ifndef GHOTI_IO_GRHEAP_STORE_H
#define GHOTI_IO_GRHEAP_STORE_H

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/core.h>
#include <ghoti.io/runtime-heap/heap.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Stores a raw pointer into a slot.
 *
 * @param heap The heap.
 * @param holder The object containing `slot`, or NULL (see the file notes).
 * @param slot The slot to write.
 * @param value The pointer to a payload start, or NULL.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for a NULL heap or slot, or a
 *   non-owner (nothing is written).
 */
GRHEAP_API GRHEAP_Result grheap_store(
    GRHEAP_Heap * heap, void * holder, void ** slot, void * value);

/**
 * @brief Stores `count` raw pointers into consecutive slots.
 *
 * The ranges may overlap (the copy is as `memmove`). Counts `count` stores.
 *
 * @param heap The heap.
 * @param holder The object containing `dst`, or NULL.
 * @param dst The first slot to write.
 * @param src The first value to copy.
 * @param count How many.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for a NULL heap, or a NULL
 *   `dst` or `src` with a non-zero count, or a non-owner.
 */
GRHEAP_API GRHEAP_Result grheap_store_range(GRHEAP_Heap * heap, void * holder,
    void ** dst, void * const * src, size_t count);

/**
 * @brief Stores an encoded value into a 64-bit slot.
 *
 * @param heap The heap.
 * @param holder The object containing `slot`, or NULL.
 * @param slot The slot to write.
 * @param value The word, in the heap's value encoding.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for a NULL heap or slot, or a
 *   non-owner.
 */
GRHEAP_API GRHEAP_Result grheap_store_word(
    GRHEAP_Heap * heap, void * holder, uint64_t * slot, uint64_t value);

/**
 * @brief Walks every live object's traced slots and every weak and ephemeron
 *   slot, and counts those whose value differs from what the last
 *   `grheap_store*` wrote (a slot never stored to must be zero).
 *
 * With verify off (::GRHEAP_VERIFY_OFF) nothing was recorded, so nothing is
 * checked: it succeeds with zero violations. In ::GRHEAP_VERIFY_ABORT mode
 * the first violation prints the object's type name and the slot to stderr and
 * aborts. Violations are also added to `verify_violations` in the stats.
 *
 * @param heap The heap.
 * @param out_violations Receives the violations this walk found. Written only
 *   on success.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for a NULL argument, a non-owner
 *   or a call from inside a `trace`, `destroy` or `cleared` function;
 *   ::GRHEAP_ERR_OOM if the shadow could not keep up with the stores (its
 *   allocation failed), so a comparison would prove nothing.
 */
GRHEAP_API GRHEAP_Result grheap_verify(
    GRHEAP_Heap * heap, uint64_t * out_violations);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRHEAP_STORE_H */
