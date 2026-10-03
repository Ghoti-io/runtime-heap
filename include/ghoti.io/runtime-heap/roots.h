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
 * @file roots.h
 * @stability stable
 *
 * How a pointer is kept alive: registered root slots, handles and pins.
 *
 * A GC pointer held outside the heap and outside a root source's stack must
 * be one of these (AD-11). A raw pointer in host code is otherwise valid only
 * on the owner thread and only until the next allocation. In arena mode all
 * three are accepted and keep nothing alive, since nothing is freed one at a
 * time.
 *
 * Threads: owner only. Handles and the heap migrate with the context.
 */

#ifndef GHOTI_IO_GRHEAP_ROOTS_H
#define GHOTI_IO_GRHEAP_ROOTS_H

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/core.h>
#include <ghoti.io/runtime-heap/heap.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A handle: a heap-owned cell that holds one object pointer and keeps
 *   its target alive. Opaque.
 *
 * A handle is freed by ::grheap_handle_destroy or when its context is
 * destroyed, whichever is first; using one after that is a use after free.
 */
typedef struct GRHEAP_Handle GRHEAP_Handle;

/**
 * @brief Registers a slot as a root: whatever pointer it holds at collection
 *   time is kept alive.
 *
 * The slot is read, never written, and must stay valid until it is removed.
 * It holds a raw pointer to a payload start, or NULL; anything else is
 * counted in `invalid_pointers` at each collection and skipped.
 *
 * @param heap The heap.
 * @param slot The slot.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for a NULL argument, a non-owner,
 *   a slot already registered, or a call during a collection;
 *   ::GRHEAP_ERR_LIMIT or ::GRHEAP_ERR_OOM as for ::grheap_alloc.
 */
GRHEAP_API GRHEAP_Result grheap_root_add(GRHEAP_Heap * heap, void ** slot);

/**
 * @brief Unregisters a root slot.
 *
 * @param heap The heap.
 * @param slot The slot, as it was added.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for a NULL argument, a
 *   non-owner or a slot that is not registered.
 */
GRHEAP_API GRHEAP_Result grheap_root_remove(GRHEAP_Heap * heap, void ** slot);

/**
 * @brief Creates a handle holding `object`.
 *
 * @param heap The heap.
 * @param object A live payload start, or NULL for an empty handle.
 * @param out_handle Receives the handle. Written only on success.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for a NULL heap or output, a
 *   non-owner, a non-NULL `object` that is not a live payload start, or a
 *   call during a collection; ::GRHEAP_ERR_LIMIT or ::GRHEAP_ERR_OOM as for
 *   ::grheap_alloc. A refusal leaves nothing behind.
 */
GRHEAP_API GRHEAP_Result grheap_handle_create(
    GRHEAP_Heap * heap, void * object, GRHEAP_Handle ** out_handle);

/**
 * @brief The object a handle holds.
 *
 * @param handle The handle.
 * @return The payload, or NULL for an empty handle, a NULL handle or a
 *   non-owner.
 */
GRHEAP_API void * grheap_handle_get(const GRHEAP_Handle * handle);

/**
 * @brief Points a handle at another object.
 *
 * @param handle The handle.
 * @param object A live payload start, or NULL.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for a NULL handle, a
 *   non-owner, or an `object` that is not a live payload start (the handle is
 *   unchanged).
 */
GRHEAP_API GRHEAP_Result grheap_handle_set(GRHEAP_Handle * handle, void * object);

/**
 * @brief Destroys a handle; its target is no longer kept alive by it.
 *
 * @param handle The handle.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for NULL, a non-owner or a call
 *   during a collection (the handle is then still valid).
 */
GRHEAP_API GRHEAP_Result grheap_handle_destroy(GRHEAP_Handle * handle);

/**
 * @brief Pins an object: it stays alive, and (in a later moving mode) in
 *   place, until it is unpinned as many times as it was pinned.
 *
 * Pins are counted per object, up to ::GRHEAP_PIN_MAX.
 *
 * @param heap The heap.
 * @param object A live payload start.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for a NULL argument, a non-owner,
 *   an `object` that is not a live payload start, or a call during a
 *   collection; ::GRHEAP_ERR_LIMIT when the object already has
 *   ::GRHEAP_PIN_MAX pins, or when the context's memory budget refused the
 *   count table; ::GRHEAP_ERR_OOM for any other allocation failure. A refusal
 *   changes nothing.
 */
GRHEAP_API GRHEAP_Result grheap_pin(GRHEAP_Heap * heap, void * object);

/**
 * @brief Drops one pin.
 *
 * @param heap The heap.
 * @param object A live payload start that has a pin.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for a NULL argument, a
 *   non-owner, an `object` that is not a live payload start, or one with no
 *   pin.
 */
GRHEAP_API GRHEAP_Result grheap_unpin(GRHEAP_Heap * heap, void * object);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRHEAP_ROOTS_H */
