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
 * @file heap.h
 * @stability stable
 *
 * The heap: creating it, allocating from it, collecting it and asking what it
 * holds.
 *
 * One heap is attached to one `runtime-core` context, through a
 * cardinality-one key in the poll's ACT phase (AD-19). Whoever configures the
 * context creates its heap (AD-20); engines attach to it and never create or
 * destroy one. There is no public heap destroy: the heap dies with its
 * context. It allocates its objects from the context's counting page provider
 * and everything else from the context's counting allocator, so the context's
 * meter is exact (AD-13), and destroying the context frees all of it.
 *
 * Objects are described by ::GRHEAP_Type and are never moved (the first
 * implementation of AD-11 is a non-moving, stop-the-world mark and sweep;
 * moving is designed for, not built). An object has a 16-byte header, a
 * payload that starts 16-byte aligned, and at least one pad byte, so a
 * pointer one past the end of the payload still lies inside the object. The
 * payload is zero-filled when allocated.
 *
 * Who owns what. The heap owns every object it returns, and a caller never
 * frees one. The pointer ::grheap_alloc returns is *borrowed*: it is valid
 * until a collection finds the object unreachable. The ways to keep an object
 * reachable are a registered root slot, a handle, a pin, a pointer stored into
 * another reachable object with ::grheap_store, a precise slot or a
 * conservative range reported by one of the context's root sources, and
 * nothing else (AD-11). Every allocation is a GC point (AD-17): an object not
 * kept by one of those may be gone when the next allocation returns.
 *
 * What else lives with the heap. A type may name a weak cell or an ephemeron
 * (::GRHEAP_WeakSpec, in type.h), which a collection clears and reports after
 * the sweep; id.h gives an object a stable ID and an identity hash that do not
 * depend on its address; and two instruments, off by default, find the mistakes
 * the rules above make easy: GC torture collects at every GC point the heap
 * owns and poisons every freed cell, so a root that is missing shows itself, and
 * barrier-verify finds a pointer written without ::grheap_store (options.h,
 * store.h). A collection runs: verify, mark, the ephemeron fixpoint, clear weak
 * slots, forget dead IDs, sweep, then the `cleared` callbacks.
 *
 * Threads: a heap belongs to its context's owning thread. Every function takes
 * the owner and refuses any other thread with ::GRHEAP_ERR_INVALID (or its
 * documented equivalent for a query). The heap and its handles migrate with
 * the context, by `grcore_context_release` and `grcore_context_acquire`.
 */

#ifndef GHOTI_IO_GRHEAP_HEAP_H
#define GHOTI_IO_GRHEAP_HEAP_H

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/core.h>
#include <ghoti.io/runtime-heap/options.h>
#include <ghoti.io/runtime-heap/type.h>

#include <ghoti.io/runtime-core/b/context.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A heap. Opaque; owned by its context. */
typedef struct GRHEAP_Heap GRHEAP_Heap;

/** @brief Bytes of header in front of every payload. */
#define GRHEAP_HEADER_BYTES 16u

/**
 * @brief The largest cell, in bytes, served from a size-class block. An object
 *   whose header, payload and pad byte, rounded up to 16 bytes, are larger
 *   than this gets a page-rounded mapping of its own.
 */
#define GRHEAP_SMALL_CELL_MAX 8192u

/** @brief The most pins one object accepts; the next is ::GRHEAP_ERR_LIMIT. */
#define GRHEAP_PIN_MAX 255u

/**
 * @brief What a heap has done and holds. A snapshot, read with
 *   ::grheap_stats.
 *
 * "Bytes" of live objects are whole cells (header, payload, pad byte and
 * rounding), which is what the heap's footprint and its threshold are
 * measured in; a payload size is read with ::grheap_size_of.
 */
typedef struct GRHEAP_Stats {
  uint64_t live_objects;      ///< Objects allocated and not yet freed.
  uint64_t live_bytes;        ///< Cell bytes of those objects.
  uint64_t mapped_bytes;      ///< Bytes mapped from the page provider.
  uint64_t collections;       ///< Collections run, of any trigger.
  uint64_t allocations;       ///< Objects ever allocated.
  uint64_t frees;             ///< Objects ever freed.
  uint64_t stores;            ///< Slots written through the `grheap_store`s.
  uint64_t handles;           ///< Handles now existing.
  uint64_t pins;              ///< Pins now outstanding, counting repeats.
  uint64_t roots;             ///< Root slots now registered.
  uint64_t conservative_hits; ///< Words in conservative ranges that named an
                              ///< object.
  uint64_t invalid_pointers;  ///< Precise slots that were not a payload start
                              ///< and were skipped.
  uint64_t torture_collections; ///< Collections torture ran (allocation starts
                              ///< and polls), a subset of `collections`.
  uint64_t verify_violations; ///< Barrier-verify violations found, counting a
                              ///< slot again at every walk that finds it.
  uint64_t weak_cleared;      ///< Weak cells whose slot a collection cleared.
  uint64_t ephemerons_cleared; ///< Ephemerons a collection cleared.
  uint64_t ids_issued;        ///< Stable IDs ever assigned.
  uint64_t ids_live;          ///< Stable IDs whose object is still alive.
} GRHEAP_Stats;

/**
 * @brief Creates the context's heap and attaches it.
 *
 * Registers the heap under a cardinality-one key, so a second call on the
 * same context is refused and leaves the first heap intact. Allowed when the
 * context is parked, at-poll or paused; not while it is running or being
 * destroyed.
 *
 * @param context The context. The caller must own it.
 * @param options The options, or NULL for the defaults. Read once, copied.
 * @param out_heap Receives the heap, owned by the context. Written only on
 *   success.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for a NULL context or output, a
 *   non-owner, a context that already has a heap or cannot register now;
 *   ::GRHEAP_ERR_LIMIT when the context's memory budget refused an
 *   allocation; ::GRHEAP_ERR_OOM for any other allocation failure. A refusal
 *   changes nothing.
 */
GRHEAP_API GRHEAP_Result grheap_heap_create(GRCORE_Context * context,
    const GRHEAP_Options * options, GRHEAP_Heap ** out_heap);

/**
 * @brief The heap attached to a context.
 *
 * Owner only; not checked, to keep the lookup cheap.
 *
 * @param context The context.
 * @return The heap, or NULL if none was created (or `context` is NULL).
 */
GRHEAP_API GRHEAP_Heap * grheap_heap_get(const GRCORE_Context * context);

/**
 * @brief Allocates an object of the type's default size.
 *
 * The payload is zero-filled. May collect first (see the allocation
 * threshold) and, when a block or mapping cannot be obtained, collects once
 * and tries again. The object is not reachable from anywhere until the caller
 * stores it, roots it, wraps it in a handle or pins it.
 *
 * @param heap The heap.
 * @param type The type, by address. Must be valid (::grheap_type_valid) and
 *   outlive the object.
 * @param out_object Receives the payload. Borrowed. Written only on success.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for a NULL argument, an invalid
 *   type (`struct_size`), a non-owner, or an allocation during a collection
 *   or from inside a `trace` or `destroy` function; ::GRHEAP_ERR_LIMIT when the context's memory budget
 *   refused the memory (the context's refusal count rose during the attempt);
 *   ::GRHEAP_ERR_OOM otherwise. A failed allocation changes nothing the
 *   caller can see, apart from a collection that may have run.
 */
GRHEAP_API GRHEAP_Result grheap_alloc(
    GRHEAP_Heap * heap, const GRHEAP_Type * type, void ** out_object);

/**
 * @brief Allocates an object of `bytes` payload bytes instead of the type's
 *   default (an array, a string).
 *
 * As ::grheap_alloc. The `trace` function still sees only the payload, so a
 * variable-sized object keeps its own length in the payload.
 *
 * @param heap The heap.
 * @param type The type, by address.
 * @param bytes The payload size; zero is allowed.
 * @param out_object Receives the payload. Written only on success.
 * @return As ::grheap_alloc; a size too large to represent is
 *   ::GRHEAP_ERR_OOM.
 */
GRHEAP_API GRHEAP_Result grheap_alloc_sized(GRHEAP_Heap * heap,
    const GRHEAP_Type * type, size_t bytes, void ** out_object);

/**
 * @brief Runs a full collection now.
 *
 * Roots, in order: the registered root slots, the handles, the pinned
 * objects, then the context's root sources. Everything not reached is freed
 * (its `destroy` hook runs first). In arena mode this does nothing and
 * succeeds.
 *
 * @param heap The heap.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for NULL, a non-owner, or a call
 *   from inside a `trace` or `destroy` function. A collection does not fail.
 */
GRHEAP_API GRHEAP_Result grheap_collect(GRHEAP_Heap * heap);

/**
 * @brief Arena mode: runs every object's `destroy` hook and releases all of
 *   the heap's memory, keeping the heap.
 *
 * Every pointer to an object is dead afterwards. Handles are cleared to NULL
 * and pins are dropped; registered root slots stay registered (and are not
 * written). Counts as a free for every object.
 *
 * @param heap The heap.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for NULL, a non-owner, a heap not
 *   in arena mode, or a call from inside a `trace` or `destroy` function.
 */
GRHEAP_API GRHEAP_Result grheap_arena_reset(GRHEAP_Heap * heap);

/**
 * @brief Whether `pointer` is exactly the payload start of a live object of
 *   this heap.
 *
 * An interior pointer, a pointer one past the end, a freed object and a
 * foreign address are all false.
 *
 * @param heap The heap.
 * @param pointer The address to test.
 * @return True for a live payload start; false otherwise, for NULL, and from
 *   a non-owner.
 */
GRHEAP_API bool grheap_contains(const GRHEAP_Heap * heap, const void * pointer);

/**
 * @brief The type an object was allocated with.
 *
 * @param heap The heap.
 * @param object A live payload start.
 * @return The type, or NULL if `object` is not one (see ::grheap_contains).
 */
GRHEAP_API const GRHEAP_Type * grheap_type_of(
    const GRHEAP_Heap * heap, const void * object);

/**
 * @brief The payload size an object was allocated with.
 *
 * @param heap The heap.
 * @param object A live payload start.
 * @param out_size Receives the size in bytes. Written only on success.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for a NULL argument, a
 *   non-owner or an `object` that is not a live payload start.
 */
GRHEAP_API GRHEAP_Result grheap_size_of(
    const GRHEAP_Heap * heap, const void * object, size_t * out_size);

/**
 * @brief Reads the heap's counters.
 *
 * @param heap The heap.
 * @param out_stats Receives a snapshot. Written only on success.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for a NULL argument or a
 *   non-owner.
 */
GRHEAP_API GRHEAP_Result grheap_stats(
    const GRHEAP_Heap * heap, GRHEAP_Stats * out_stats);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRHEAP_HEAP_H */
