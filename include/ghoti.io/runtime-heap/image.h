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
 * @file image.h
 * @stability stable
 *
 * Heap images: a frozen, shareable copy of the objects a context can reach
 * (AD-11, AD-12; the spine's Reserved Seams, context snapshots).
 *
 * An image is *data about objects*, not a copy of memory. It holds, for every
 * object reachable from the context's root sources, the type's *name*, the
 * payload bytes with every slot the type's `trace` reports turned into an
 * *index* into the image, the stable ID if the object had one, and whatever
 * extra bytes the type's `snapshot` hook wrote for a host pointer it cleared
 * (type.h). It also holds, for every precise root slot the context's sources
 * report, either a plain word or an index. It holds no address of any kind, so
 * an image can be restored into any heap in any process layout, any number of
 * times, concurrently, and may outlive the heap it came from.
 *
 * It is made by the same walk the collector trusts: the roots a context's
 * sources report, and each type's `trace`. A type that is correct for the
 * collector is correct for an image; the one new duty is naming what is not
 * heap. The image is an in-memory object; a byte format or a file is not done.
 *
 * **What refuses a take.** Each is ::GRHEAP_ERR_INVALID, and nothing changes:
 * a registered root slot (a C pointer the heap cannot rewrite), a handle, a
 * pin, a weak cell or an ephemeron in the heap (any object whose type has a
 * `weak` spec, or any registered one), a conservative range that a root source
 * reports as non-empty, an object whose type has no name or shares a name with
 * a different type, a slot reported outside its payload, a pointer word that
 * the heap's codec cannot encode back, and a hook that refuses. Roots and
 * stable IDs that name a collected object are not in the image.
 *
 * **Instantiating.** ::grheap_image_instantiate makes a fresh object for each
 * entry, through the heap's own allocator and the context's memory budget,
 * resolves the type names through a host callback, patches every slot, runs the
 * types' `restore` hooks, restores the stable IDs and the next ID to issue (so
 * a later ID cannot collide), and writes the root slots by enumerating the
 * context's root sources again, which must report the same number of slots as
 * at the take. It is refused into a heap that already holds objects or IDs.
 * Any failure leaves a collected heap exactly as it was. An arena heap cannot
 * give single objects back, so after a failed restore it keeps the cells (and
 * their accounting) the restore had made; they are unreachable, and a retry
 * needs a fresh heap or ::grheap_arena_reset.
 *
 * The collector is unchanged: torture and barrier-verify run over a restored
 * heap as over any other (every restored reference slot is recorded as stored
 * through the barrier).
 *
 * Threads: taking and instantiating are the heap's owner's. An image's retain,
 * release and queries are safe from any thread.
 */

#ifndef GHOTI_IO_GRHEAP_IMAGE_H
#define GHOTI_IO_GRHEAP_IMAGE_H

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/allocator.h>
#include <ghoti.io/runtime-heap/core.h>
#include <ghoti.io/runtime-heap/heap.h>
#include <ghoti.io/runtime-heap/type.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An immutable, reference-counted heap image. Opaque. */
typedef struct GRHEAP_Image GRHEAP_Image;

/** @brief The name the heap's key goes by in a context snapshot (the
 *   environment lookup of `grcore_context_restore` is asked for it). */
#define GRHEAP_SNAPSHOT_KEY_NAME "runtime-heap"

/**
 * @brief Finds the destination's type of a name, for an image restored into a
 *   heap.
 *
 * A type is identified across heaps by its `name`, which is why a type that is
 * to be in an image must have one and share it with no other.
 *
 * @param user The environment's `user`.
 * @param name The name an object was allocated under.
 * @return The type, or NULL if the destination has none of that name (which
 *   refuses the restore).
 */
typedef const GRHEAP_Type * (*GRHEAP_TypeResolver)(
    void * user, const char * name);

/**
 * @brief What the host supplies to an instantiation: how to find the types by
 *   name, and the `user` its `restore` hooks and resolver are given.
 *
 * This is also the environment the heap's key is given in a context restore
 * (see ::GRHEAP_SNAPSHOT_KEY_NAME).
 */
typedef struct GRHEAP_RestoreEnv {
  GRHEAP_TypeResolver resolve; ///< Required.
  void * user;                 ///< Handed to `resolve` and to every `restore`.
} GRHEAP_RestoreEnv;

/**
 * @brief Takes an image of the objects reachable from the context's roots.
 *
 * @param heap The heap. The caller must own it, and it must not be inside a
 *   collection, a `trace` or a `destroy` function.
 * @param allocator Allocates the image, which keeps a copy of it; NULL means
 *   the default. It is not the context's: an image may outlive the context.
 * @param out_image Receives the image with a count of one. Written only on
 *   success.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for a NULL argument, a non-owner, a
 *   busy heap, or any of the refusals above; ::GRHEAP_ERR_OOM or
 *   ::GRHEAP_ERR_LIMIT when the allocator refused. Nothing is changed and
 *   nothing is left allocated.
 */
GRHEAP_API GRHEAP_Result grheap_image_take(GRHEAP_Heap * heap,
    const GRHEAP_Allocator * allocator, GRHEAP_Image ** out_image);

/**
 * @brief Restores an image into an empty heap.
 *
 * @param heap The heap. The caller must own it; it holds no object and no
 *   stable ID, its value codec equals the image's, and the context's root
 *   sources report as many precise slots as they did at the take and no
 *   conservative range.
 * @param image The image.
 * @param env The resolver and `user`.
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_INVALID for a NULL argument, a non-owner, a
 *   heap that is not empty or busy, a codec or root layout that differs, a type
 *   name `env->resolve` does not know, a resolved type that has a `weak` spec or
 *   is given extra bytes it has no `restore` for, or a `restore` hook that
 *   refuses; ::GRHEAP_ERR_LIMIT when the context's memory budget refused;
 *   ::GRHEAP_ERR_OOM for any other allocation failure. On any failure a
 *   collected heap is as it was (an arena heap keeps the unreachable cells).
 */
GRHEAP_API GRHEAP_Result grheap_image_instantiate(GRHEAP_Heap * heap,
    const GRHEAP_Image * image, const GRHEAP_RestoreEnv * env);

/** @brief Adds a reference. Safe from any thread. NULL is a no-op. */
GRHEAP_API GRHEAP_Image * grheap_image_retain(GRHEAP_Image * image);

/** @brief Drops a reference; at zero, frees the image. NULL is a no-op. */
GRHEAP_API void grheap_image_release(GRHEAP_Image * image);

/** @brief The current count (a reading); zero for NULL. */
GRHEAP_API size_t grheap_image_refcount(const GRHEAP_Image * image);

/** @brief The image's size in bytes; zero for NULL. */
GRHEAP_API size_t grheap_image_size(const GRHEAP_Image * image);

/** @brief How many objects the image holds; zero for NULL. */
GRHEAP_API size_t grheap_image_object_count(const GRHEAP_Image * image);

/** @brief How many precise root slots the image recorded; zero for NULL. */
GRHEAP_API size_t grheap_image_root_count(const GRHEAP_Image * image);

/**
 * @brief The image's bytes, for a test that wants to prove what an image holds
 *   (for example that it holds no address) and for a context snapshot, which
 *   carries them as one blob.
 *
 * @param out_size Receives the length. May be NULL.
 * @return A pointer valid as long as the image; NULL for NULL.
 */
GRHEAP_API const void * grheap_image_bytes(
    const GRHEAP_Image * image, size_t * out_size);

/**
 * @brief Appends bytes to the extra bytes of the object a `snapshot` hook is
 *   writing.
 *
 * @return ::GRHEAP_OK; ::GRHEAP_ERR_OOM; ::GRHEAP_ERR_INVALID for NULL (with
 *   `size` non-zero). A hook returns a failure unchanged.
 */
GRHEAP_API GRHEAP_Result grheap_image_writer_write(
    GRHEAP_ImageWriter * writer, const void * data, size_t size);

/** @brief Appends a length-prefixed, NUL-terminated string; NULL is the empty one. */
GRHEAP_API GRHEAP_Result grheap_image_writer_string(
    GRHEAP_ImageWriter * writer, const char * text);

/**
 * @brief Reads the next `size` bytes a `restore` hook was given. Reading past
 *   the end fails and stays failed.
 *
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_CORRUPT past the end.
 */
GRHEAP_API GRHEAP_Result grheap_image_reader_read(
    GRHEAP_ImageReader * reader, void * out, size_t size);

/**
 * @brief Reads a string written by ::grheap_image_writer_string.
 *
 * @param out_text Receives a NUL-terminated string valid as long as the image.
 * @param out_length Receives its length; may be NULL.
 * @return As ::grheap_image_reader_read.
 */
GRHEAP_API GRHEAP_Result grheap_image_reader_string(
    GRHEAP_ImageReader * reader, const char ** out_text, size_t * out_length);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRHEAP_IMAGE_H */
