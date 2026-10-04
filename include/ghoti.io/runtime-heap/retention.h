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
 * @file retention.h
 * @stability stable
 *
 * The retention query: why is this object still alive? (AD-11, AD-12.)
 *
 * Given a live object, ::grheap_retention_path finds the *shortest chain of
 * references* that keeps it: breadth-first from every root the collector would
 * use, to the object. "Shortest" is among *ordinary* references (a root, then
 * slots a `trace` reports): an ephemeron route is considered only when no
 * ordinary route exists, so a longer ordinary chain wins over a shorter one that
 * needs an ephemeron hop. The answer is a root description followed by one step per
 * object on the chain, the last step being the object asked about.
 *
 * It uses what the collector uses and nothing else: the registered root slots,
 * the handles, the pins, the context's root sources (each asked on its own
 * through `grcore_context_root_source`, so the answer can name the source and
 * the slot's ordinal in its enumeration), the conservative ranges, and each
 * type's `trace`. A *weak* slot is never an edge. An ephemeron's value is
 * reached through its key exactly as the collector's fixpoint reaches it, and
 * that hop is marked in the result, so an object kept alive only that way is
 * still explained. Ties are broken in the collector's own root and slot order,
 * so the answer is deterministic.
 *
 * **An object no root reaches** (garbage not yet swept) is a distinct answer:
 * the call succeeds and ::grheap_retention_retained is false. It is never an
 * empty path presented as success and never an error.
 *
 * **When.** Only the owning thread, and only when the context is parked, paused
 * or at a poll (a handler may call it; it reads and changes nothing, so no
 * phase can be harmed, and an OBSERVE or YIELD handler is where it is meant to
 * be called), never while the context runs between polls and never while the
 * heap is busy (inside a collection, an allocation, a `trace` or a `destroy`).
 * A pointer that is not exactly the start of a live payload (interior, one past
 * the end, freed, foreign, NULL) is refused as invalid with nothing changed.
 *
 * **What it does not do.** It runs no collection, moves nothing, pins nothing
 * and marks nothing: no mark bit, pin, ID or statistic changes. It allocates
 * nothing *in the GC heap*; its scratch (a visited set and a queue) comes from
 * the context's counting allocator like any host allocation and is released
 * before it returns (the *result* is made with the caller's allocator, below), and at every allocation failure it ends as an error,
 * leaks nothing and leaves the heap unchanged.
 *
 * **No identity in the result.** AD-12 keys nothing by address, so the result
 * stores none as an identity: the addresses in it are valid only until the heap
 * next collects or the context next runs. A host that wants something durable
 * calls ::grheap_id_of for the steps it cares about.
 *
 * **Lifetime of the result.** It is made with the allocator the caller gives
 * (NULL: the default) and released with ::grheap_retention_release; it does not
 * depend on the heap or the context once made. The root's `source` name is the
 * string the root source gave and lives as long as that source.
 */

#ifndef GHOTI_IO_GRHEAP_RETENTION_H
#define GHOTI_IO_GRHEAP_RETENTION_H

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/allocator.h>
#include <ghoti.io/runtime-heap/core.h>
#include <ghoti.io/runtime-heap/heap.h>
#include <ghoti.io/runtime-heap/type.h>

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A retention path: a root and the steps from it. Opaque. */
typedef struct GRHEAP_Retention GRHEAP_Retention;

/** @brief What kind of root holds the first object of a path. */
typedef enum {
  /** A precise slot a context root source reports (the guest stack, for one).
   *  `source` names the source; `ordinal` counts that source's precise slots
   *  from zero in its enumeration order. */
  GRHEAP_ROOT_CONTEXT_SOURCE = 0,
  /** A slot registered with ::grheap_root_add. `ordinal` is its position in
   *  registration order. */
  GRHEAP_ROOT_C_ROOT,
  /** A handle. `ordinal` is its position in the order the collector visits
   *  them (the most recently created first). */
  GRHEAP_ROOT_HANDLE,
  /** A pinned object. `ordinal` counts pinned objects in the collector's
   *  order (by span, then cell). Not found in an arena heap, which keeps no
   *  per-object pins. */
  GRHEAP_ROOT_PIN,
  /** A word inside a conservative range a context root source reports.
   *  `source` names the source, `ordinal` counts that source's ranges from
   *  zero, and `offset` is the word's byte offset from the range's `lo`. The
   *  word may point anywhere inside the object. */
  GRHEAP_ROOT_CONSERVATIVE
} GRHEAP_RootKind;

/** @brief The root a path starts at. */
typedef struct GRHEAP_RetentionRoot {
  GRHEAP_RootKind kind;  ///< What kind of root.
  const char * source;   ///< The root source's name; NULL for other kinds, or a
                         ///< source with no name.
  size_t ordinal;        ///< Which one; see ::GRHEAP_RootKind.
  size_t offset;         ///< Conservative roots: the word's byte offset in the
                         ///< range. Zero otherwise.
} GRHEAP_RetentionRoot;

/** @brief A step's `slot_offset` when the referencing slot is not inside the
 *  holder's payload. */
#define GRHEAP_RETENTION_NOT_INLINE ((size_t)-1)

/** @brief One object on a path. */
typedef struct GRHEAP_RetentionStep {
  void * object;               ///< The object's payload. Valid only until the
                               ///< heap next collects or the context runs.
  const GRHEAP_Type * type;    ///< Its type.
  size_t slot_offset;          ///< Not the last step: the byte offset, in this
                               ///< object's payload, of the slot that
                               ///< references the next step; for an ephemeron
                               ///< hop it is the offset in the *ephemeron's*
                               ///< payload (`ephemeron`), not in this object's.
                               ///< ::GRHEAP_RETENTION_NOT_INLINE when the slot
                               ///< a `trace` reported is not inside the payload
                               ///< (out-of-line storage). The last step: zero.
  void * ephemeron;            ///< NULL for a plain reference. Otherwise the
                               ///< reference to the next step is an ephemeron
                               ///< hop: this object is the ephemeron's key
                               ///< (or the ephemeron itself, for an
                               ///< immediate key), `ephemeron` is the
                               ///< ephemeron, and `slot_offset` is its value
                               ///< slot.
  bool target;                 ///< True for the last step, the object asked
                               ///< about.
} GRHEAP_RetentionStep;

/**
 * @brief Finds the shortest chain of references from a root to `object`.
 *
 * @param heap The heap.
 * @param object A live payload start.
 * @param allocator The allocator for the *result* only (the scratch is the
 *   context's); NULL for the default.
 * @param out_path Receives the result, released with
 *   ::grheap_retention_release. Written only on success.
 * @return ::GRHEAP_OK (the object may still be not retained: ask
 *   ::grheap_retention_retained); ::GRHEAP_ERR_INVALID for a NULL argument, a
 *   non-owner, a running context, a busy heap, or an `object` that is not
 *   exactly a live payload start; ::GRHEAP_ERR_LIMIT when the context's memory
 *   budget refused scratch; ::GRHEAP_ERR_OOM for any other allocation failure.
 *   A refusal changes nothing and leaks nothing.
 */
GRHEAP_API GRHEAP_Result grheap_retention_path(GRHEAP_Heap * heap,
    const void * object, const GRHEAP_Allocator * allocator,
    GRHEAP_Retention ** out_path);

/**
 * @brief Releases a result. NULL is ignored.
 *
 * @param path The result.
 */
GRHEAP_API void grheap_retention_release(GRHEAP_Retention * path);

/**
 * @brief Whether some root reaches the object.
 *
 * @param path The result.
 * @return True if a chain was found; false for an object nothing reaches (the
 *   collector would free it), and for NULL.
 */
GRHEAP_API bool grheap_retention_retained(const GRHEAP_Retention * path);

/**
 * @brief The root a retained path starts at.
 *
 * @param path The result.
 * @param out_root Receives the root. Written only on success.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for NULL or a result that is
 *   not retained.
 */
GRHEAP_API GRHEAP_Result grheap_retention_root(
    const GRHEAP_Retention * path, GRHEAP_RetentionRoot * out_root);

/**
 * @brief How many objects are on the path: at least one for a retained result,
 *   the first held by the root and the last the object asked about; zero when
 *   it is not retained.
 *
 * @param path The result.
 * @return The count; zero for NULL.
 */
GRHEAP_API size_t grheap_retention_step_count(const GRHEAP_Retention * path);

/**
 * @brief Reads one step.
 *
 * @param path The result.
 * @param index From zero (the object the root holds) to the count minus one
 *   (the target).
 * @param out_step Receives the step. Written only on success.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for NULL or an index out of
 *   range.
 */
GRHEAP_API GRHEAP_Result grheap_retention_step(const GRHEAP_Retention * path,
    size_t index, GRHEAP_RetentionStep * out_step);

/**
 * @brief The name of a root kind, for a message.
 *
 * @param kind The kind.
 * @return A static string; "unknown" for a value out of range.
 */
GRHEAP_API const char * grheap_retention_root_kind_string(GRHEAP_RootKind kind);

/**
 * @brief Writes a human-readable form of the result: the root, then one line
 *   per step (type name, slot offset, ephemeron hops). Addresses are not
 *   printed.
 *
 * @param path The result.
 * @param out Where to write.
 */
GRHEAP_API void grheap_retention_dump(const GRHEAP_Retention * path, FILE * out);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRHEAP_RETENTION_H */
