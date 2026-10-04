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
 * whatever the payload owns outside the heap. A type may also say that one
 * slot of its payload is a *weak cell* or a pair of slots an *ephemeron*
 * (::GRHEAP_WeakSpec), which the heap then handles itself.
 */

#ifndef GHOTI_IO_GRHEAP_TYPE_H
#define GHOTI_IO_GRHEAP_TYPE_H

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/core.h>

#include <ghoti.io/runtime-core/b/context.h>

#include <stdbool.h>
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
 * collection (when the mark stack overflows), so it must be idempotent. It
 * reads the object's own payload and memory the object owns, and no other heap
 * object: with barrier-verify on, it is also run on an object found dead, just
 * before that object's `destroy`, to drop the shadow of the slots it reports,
 * and by then the objects its neighbours pointed at may already have been
 * freed in the same sweep.
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
 * @brief Where a type's `snapshot` hook writes the extra bytes of one object
 *   into a heap image. Opaque; valid only for the duration of the call.
 */
typedef struct GRHEAP_ImageWriter GRHEAP_ImageWriter;

/** @brief Where a type's `restore` hook reads them back. Opaque; valid only for
 *   the duration of the call. */
typedef struct GRHEAP_ImageReader GRHEAP_ImageReader;

/**
 * @brief Prepares one object's payload for a heap image.
 *
 * An image holds no host pointer (AD-12). A type whose payload holds one (a
 * pointer to a library member, to a registry, to memory outside the heap) must
 * say what it points at by *name*, and clear the pointer: the hook is given a
 * *copy* of the payload, already byte-for-byte what the object holds, with
 * every slot `trace` reports already turned into an index, and it may change
 * that copy and write bytes of its own with ::grheap_image_writer_write. It
 * runs once per object in the image, in discovery order. It must not touch the
 * heap, the original object or the context's state.
 *
 * A type with neither this nor `restore` is copied verbatim, so its payload
 * must hold nothing but plain data and the slots `trace` reports.
 *
 * @param context The context the heap belongs to, for a hook that needs to
 *   find its engine.
 * @param payload The copy. Not the object.
 * @param writer Where the object's extra bytes go.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for an object that cannot be
 *   captured (a host value that is not named anywhere), which refuses the whole
 *   image; or ::GRHEAP_ERR_OOM and ::GRHEAP_ERR_LIMIT from the writer.
 */
typedef GRHEAP_Result (*GRHEAP_SnapshotFn)(
    GRCORE_Context * context, void * payload, GRHEAP_ImageWriter * writer);

/**
 * @brief Rebuilds one object's host pointers from the bytes `snapshot` wrote.
 *
 * Called on the destination's new object, whose payload is the copy `snapshot`
 * left and whose reference slots are already patched. It resolves names to
 * pointers (the library, the member) and stores them, reading the bytes with
 * ::grheap_image_reader_read. It runs after every object of the image exists,
 * in the image's order, with the heap busy (an allocation is refused).
 *
 * If it fails the whole instantiation is undone, and the object's `destroy`
 * hook is run on whatever the payload then holds, so `restore` must leave a
 * payload that `destroy` can take (the pointers `snapshot` cleared are still
 * cleared until it stores a resolved one).
 *
 * @param context The destination context.
 * @param payload The new object's payload.
 * @param reader The bytes `snapshot` wrote for this object.
 * @param user The `user` of the ::GRHEAP_RestoreEnv the host passed.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for a name that does not resolve
 *   at the destination, which refuses the restore.
 */
typedef GRHEAP_Result (*GRHEAP_RestoreFn)(GRCORE_Context * context,
    void * payload, GRHEAP_ImageReader * reader, void * user);

/** @brief What a weak spec describes. */
typedef enum {
  /** One slot, `target_offset`, that does not keep its target alive. */
  GRHEAP_WEAK_CELL = 0,
  /**
   * Two slots, `key_offset` and `value_offset`. The value is kept alive only
   * while the key is reachable by some other path (an ephemeron).
   */
  GRHEAP_EPHEMERON
} GRHEAP_WeakKind;

/**
 * @brief Tells the heap that a type has a weak cell or an ephemeron.
 *
 * The type's `trace` function must not report these slots: the heap handles
 * them. Every offset is in bytes from the start of the payload, 8-byte aligned,
 * and must lie inside the payload of every object allocated (a refusal with
 * ::GRHEAP_ERR_INVALID otherwise, at allocation). Write the slots with
 * ::grheap_store (or ::grheap_store_word when `encoded`) like any other.
 *
 * Collection, after marking: (1) for each live ephemeron whose key is marked,
 * the value is marked too, to a fixpoint; (2) each live weak cell whose target
 * is not marked has the slot set to NULL (word 0), and each live ephemeron
 * whose key is not marked has key and value set to NULL; (3) the sweep runs;
 * (4) then `cleared(payload)` is called once for every cell or ephemeron that
 * had a slot cleared, with the heap busy. A cell that is itself dead is
 * dropped without a call. A target that is pinned, or that a conservative
 * range found, is marked and so stays. A slot that is not NULL and does not
 * decode to the start of a live payload is counted in `invalid_pointers` and
 * cleared. An `encoded` slot whose word fails the codec's tag test is an
 * immediate value: it is left alone, and an ephemeron whose key is an
 * immediate keeps its value for as long as the ephemeron lives. Set an
 * ephemeron's key before its value: a value with a NULL key is cleared. (That
 * holds for raw-pointer slots. In an `encoded` slot a word that fails the tag
 * test is not a pointer but an immediate, which never dies, so an ephemeron
 * keyed by one keeps its value.)
 *
 * `cleared` is plain C, runs no guest code and may not allocate in the heap
 * (refused with ::GRHEAP_ERR_INVALID). It cannot resurrect anything (AD-12).
 *
 * In an arena nothing is ever collected, so nothing is cleared.
 */
typedef struct GRHEAP_WeakSpec {
  GRHEAP_WeakKind kind;     ///< Weak cell or ephemeron.
  size_t target_offset;     ///< GRHEAP_WEAK_CELL: the weak slot.
  size_t key_offset;        ///< GRHEAP_EPHEMERON: the key slot.
  size_t value_offset;      ///< GRHEAP_EPHEMERON: the value slot.
  bool encoded;             ///< Slots hold codec-encoded words, not pointers.
  void (*cleared)(void * payload); ///< Called after the sweep. May be NULL.
} GRHEAP_WeakSpec;

/**
 * @brief A type descriptor. Define it `static` (or `const`) and allocate by
 *   address; it must outlive every object allocated with it.
 */
typedef struct GRHEAP_Type {
  const char * name;      ///< For diagnostics. May be NULL.
  size_t size;            ///< Payload bytes ::grheap_alloc allocates.
  GRHEAP_TraceFn trace;   ///< Reports pointer slots. NULL: a leaf object.
  GRHEAP_DestroyFn destroy; ///< Releases external resources. May be NULL.
  const GRHEAP_WeakSpec * weak; ///< A weak cell or ephemeron. May be NULL.
  /**
   * Prepares a payload for a heap image (image.h). May be NULL: the payload is
   * then copied as it is, and the type promises it holds no host pointer. A
   * type with a `weak` spec cannot be in an image at all.
   *
   * The two image hooks are last, so a static type written before them keeps
   * working: C zero-fills the omitted initialisers.
   */
  GRHEAP_SnapshotFn snapshot;
  /** Rebuilds the host pointers `snapshot` named. May be NULL. */
  GRHEAP_RestoreFn restore;
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
