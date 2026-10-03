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
 * @file id.h
 * @stability stable
 *
 * Stable IDs and the identity hash.
 *
 * An object is never moved today but may be by a later collector (AD-11), so
 * its address is not a name for it that can be kept. A stable ID is: a 64-bit
 * number, assigned on first request, from 1 and increasing, never reused. IDs
 * hold their objects weakly: a collection forgets the ID of every object it
 * frees, before its `destroy` hook runs. The identity hash is a function of
 * the ID, so it is stable across a move and never needs the address.
 *
 * Nothing here collects. Issuing an ID that needs table space the allocator
 * refuses is ::GRHEAP_ERR_LIMIT or ::GRHEAP_ERR_OOM, with nothing changed.
 *
 * Threads: owner only. ::grheap_object_of_id is a query and may be called from
 * a `cleared` or `destroy` function; the other two are refused there.
 */

#ifndef GHOTI_IO_GRHEAP_ID_H
#define GHOTI_IO_GRHEAP_ID_H

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/core.h>
#include <ghoti.io/runtime-heap/heap.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The object's stable ID, assigning the next one if it has none.
 *
 * @param heap The heap.
 * @param object A live payload start.
 * @param out_id Receives the ID. Written only on success.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for a NULL argument, a non-owner,
 *   a busy heap or an `object` that is not a live payload start;
 *   ::GRHEAP_ERR_LIMIT when the context's budget refused table space,
 *   ::GRHEAP_ERR_OOM otherwise. A refusal issues no ID.
 */
GRHEAP_API GRHEAP_Result grheap_id_of(
    GRHEAP_Heap * heap, void * object, uint64_t * out_id);

/**
 * @brief The object that holds an ID.
 *
 * @param heap The heap.
 * @param id The ID.
 * @param out_object Receives the payload. Borrowed. Written only on success.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for a NULL argument, a
 *   non-owner, or an ID that was never issued or whose object is dead.
 */
GRHEAP_API GRHEAP_Result grheap_object_of_id(
    const GRHEAP_Heap * heap, uint64_t id, void ** out_object);

/**
 * @brief A 64-bit hash of the object's identity: a mix of its stable ID
 *   (assigned if it has none).
 *
 * Equal for the same object for as long as it lives, whether or not it moves.
 * There is no identity hash that does not go through the ID.
 *
 * @param heap The heap.
 * @param object A live payload start.
 * @param out_hash Receives the hash. Written only on success.
 * @return As ::grheap_id_of.
 */
GRHEAP_API GRHEAP_Result grheap_identity_hash(
    GRHEAP_Heap * heap, void * object, uint64_t * out_hash);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRHEAP_ID_H */
