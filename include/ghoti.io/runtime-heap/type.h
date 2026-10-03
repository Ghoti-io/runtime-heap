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
 * @file type.h
 * @stability stable
 *
 * Object descriptions: how the heap learns what an object holds.
 *
 * Objects are described, never owned layouts (AD-11). The heap assumes no
 * object layout and no value encoding: a type names the object, says how big
 * it is by default, and optionally supplies a `trace` function that reports
 * each pointer slot of a payload, and a `destroy` function that releases
 * whatever the payload owns outside the heap.
 */

#ifndef GHOTI_IO_GRHEAP_TYPE_H
#define GHOTI_IO_GRHEAP_TYPE_H

#include <ghoti.io/runtime-heap/macros.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What a `trace` function reports its slots to. Opaque, and valid only
 *   for the duration of the call.
 */
typedef struct GRHEAP_Tracer GRHEAP_Tracer;

/**
 * @brief Reports every pointer slot of one object to the tracer.
 *
 * Call ::grheap_trace_slot for a slot holding a raw pointer and
 * ::grheap_trace_word for a slot holding an encoded value. Must not allocate
 * in the heap (that is refused with ::GRHEAP_ERR_INVALID), store, or run
 * guest code. It may be called more than once for one object in one
 * collection (when the mark stack overflows), so it must be idempotent.
 *
 * @param tracer The tracer.
 * @param payload The object's payload.
 */
typedef void (*GRHEAP_TraceFn)(GRHEAP_Tracer * tracer, void * payload);

/**
 * @brief Releases what the payload owns outside the heap.
 *
 * Plain C: it may not touch another heap object (which may already be
 * destroyed, in an unspecified order), and may not allocate in the heap
 * (refused with ::GRHEAP_ERR_INVALID). It cannot resurrect the object (AD-12).
 * It runs once per object: when a collection finds the object unreachable,
 * when an arena is reset, or when the context is destroyed, whichever comes
 * first.
 *
 * @param payload The object's payload; its memory is still valid.
 */
typedef void (*GRHEAP_DestroyFn)(void * payload);

/**
 * @brief A type descriptor. Define it `static` (or `const`) and allocate by
 *   address; it must outlive every object allocated with it.
 */
typedef struct GRHEAP_Type {
  const char * name;      ///< For diagnostics. May be NULL.
  size_t size;            ///< Payload bytes ::grheap_alloc allocates.
  GRHEAP_TraceFn trace;   ///< Reports pointer slots. NULL: a leaf object.
  GRHEAP_DestroyFn destroy; ///< Releases external resources. May be NULL.
} GRHEAP_Type;

/**
 * @brief Reports a slot that holds a raw pointer to the start of another
 *   object's payload.
 *
 * NULL is allowed and ignored. Anything else must be exactly the start of the
 * payload of a live object of this heap: an interior pointer, a pointer one
 * past the end, a pointer to a freed object or to memory the heap does not
 * own is counted in the heap's `invalid_pointers` and skipped. Precise slots
 * are never guessed at; only a conservative range from a root source is.
 *
 * @param tracer The tracer given to the `trace` function. NULL is ignored.
 * @param slot The address of the slot. NULL is ignored.
 */
GRHEAP_API void grheap_trace_slot(GRHEAP_Tracer * tracer, void ** slot);

/**
 * @brief Reports a slot that holds an encoded value, which is decoded with
 *   the heap's value codec (::GRHEAP_ValueCodec).
 *
 * A word that fails the codec's tag test is not a pointer and is ignored
 * without comment. A word that passes it must decode to the start of a live
 * object's payload, or is counted in `invalid_pointers` and skipped; an
 * address of zero is ignored.
 *
 * @param tracer The tracer given to the `trace` function. NULL is ignored.
 * @param slot The address of the slot. NULL is ignored.
 */
GRHEAP_API void grheap_trace_word(GRHEAP_Tracer * tracer, uint64_t * slot);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRHEAP_TYPE_H */
