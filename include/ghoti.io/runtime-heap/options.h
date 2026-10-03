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
 * @file options.h
 * @stability stable
 *
 * Heap options: an opaque object configured through setters.
 *
 * Options are an opaque setter object rather than a struct with a `_default()`
 * (CONVENTIONS.md section 13): a heap is a long-lived property of a context
 * that later work will keep extending, and a public struct would break every
 * caller each time a field was added. The same choice runtime-core makes for
 * its own options (AD-13).
 *
 * Options are read once, by ::grheap_heap_create, which copies what it needs.
 * An options object may be destroyed, or reused for another heap, afterwards.
 *
 * Threads: an options object is used from one thread at a time.
 */

#ifndef GHOTI_IO_GRHEAP_OPTIONS_H
#define GHOTI_IO_GRHEAP_OPTIONS_H

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/allocator.h>
#include <ghoti.io/runtime-heap/core.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The heap options object. Opaque. */
typedef struct GRHEAP_Options GRHEAP_Options;

/** @brief How a heap manages its objects. */
typedef enum {
  /** Stop-the-world, non-moving mark and sweep (the default). */
  GRHEAP_MODE_COLLECTED = 0,
  /**
   * Bump allocation that never collects. Objects live until
   * ::grheap_arena_reset or until the context is destroyed, and every
   * object's `destroy` hook runs then. Handles, pins and roots are accepted
   * and keep nothing alive, since nothing is ever freed one at a time.
   */
  GRHEAP_MODE_ARENA
} GRHEAP_Mode;

/**
 * @brief How a 64-bit word is decoded into the address of an object.
 *
 * A word is a pointer if and only if `(word & tag_mask) == tag_value`, and
 * then the address it names is `((word & mask) >> shift) + base`. The heap
 * does not know an engine's value encoding (AD-11, AD-18), so the engine states
 * it here as data: the same mask, shift and base a conservative range carries,
 * plus a tag test for slots whose words may be integers.
 *
 * Applies to the precise slots only: the slots a root source reports and the
 * words an object's `trace` function reports with ::grheap_trace_word. A
 * conservative range carries its own mask, shift and base and has no tag.
 *
 * The default is the identity: every word is a pointer, `tag_mask` and
 * `tag_value` are zero (which is how "no tag test" is spelled, since
 * `(word & 0) == 0` is always true), `mask` is all ones, `shift` and `base`
 * are zero.
 */
typedef struct GRHEAP_ValueCodec {
  uint64_t tag_mask;  ///< Bits of a word the tag test looks at.
  uint64_t tag_value; ///< What those bits must equal. Within `tag_mask`.
  uint64_t mask;      ///< Bits of a word that carry the address.
  unsigned shift;     ///< How far to shift them down; below 64.
  uint64_t base;      ///< Added after the shift.
} GRHEAP_ValueCodec;

/** @brief How much the barrier-verify instrument does. */
typedef enum {
  /** Nothing is recorded or checked (the default). */
  GRHEAP_VERIFY_OFF = 0,
  /** Violations are counted (`verify_violations` in the stats). */
  GRHEAP_VERIFY_COUNT,
  /** The first violation names the object and slot on stderr and aborts. */
  GRHEAP_VERIFY_ABORT
} GRHEAP_Verify;

/** @brief The default allocation threshold: 256 KiB. */
#define GRHEAP_DEFAULT_GC_THRESHOLD 262144u

/**
 * @brief Sets `*codec` to the identity codec (see ::GRHEAP_ValueCodec).
 *
 * @param codec The struct to initialise. NULL is ignored.
 */
GRHEAP_API void grheap_value_codec_default(GRHEAP_ValueCodec * codec);

/**
 * @brief Creates an options object holding the defaults: collected mode, the
 *   identity codec, a threshold of ::GRHEAP_DEFAULT_GC_THRESHOLD,
 *   auto-collect on, torture off and verify off.
 *
 * The two instruments take their default from the environment, so that a whole
 * test suite can be run under them without editing it: `GRHEAP_TORTURE=1`
 * makes torture on, and `GRHEAP_VERIFY=1` makes verify ::GRHEAP_VERIFY_ABORT
 * (`GRHEAP_VERIFY=count` makes it ::GRHEAP_VERIFY_COUNT). An explicit setter
 * always wins. A heap created with a NULL options argument gets the same
 * defaults.
 *
 * @param allocator The allocator for the object, or NULL for the default. It
 *   must outlive the object.
 * @param out_options Receives the object. Written only on success.
 * @return ::GRHEAP_OK, ::GRHEAP_ERR_INVALID for a NULL output, or
 *   ::GRHEAP_ERR_OOM.
 */
GRHEAP_API GRHEAP_Result grheap_options_create(
    const GRHEAP_Allocator * allocator, GRHEAP_Options ** out_options);

/**
 * @brief Destroys an options object.
 *
 * @param options The object, or NULL (ignored).
 */
GRHEAP_API void grheap_options_destroy(GRHEAP_Options * options);

/**
 * @brief Chooses collected or arena mode.
 *
 * @param options The object.
 * @param mode The mode.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for NULL or a value outside
 *   the enum (nothing changes).
 */
GRHEAP_API GRHEAP_Result grheap_options_set_mode(
    GRHEAP_Options * options, GRHEAP_Mode mode);

/**
 * @brief The mode.
 *
 * @param options The object.
 * @return The mode; ::GRHEAP_MODE_COLLECTED for NULL.
 */
GRHEAP_API GRHEAP_Mode grheap_options_get_mode(const GRHEAP_Options * options);

/**
 * @brief Sets how words are decoded into addresses (see
 *   ::GRHEAP_ValueCodec).
 *
 * @param options The object.
 * @param codec The codec; copied.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for NULL, a `shift` of 64 or
 *   more, or a `tag_value` with bits outside `tag_mask` (a tag that no word
 *   could ever satisfy). Nothing changes on a refusal.
 */
GRHEAP_API GRHEAP_Result grheap_options_set_value_codec(
    GRHEAP_Options * options, const GRHEAP_ValueCodec * codec);

/**
 * @brief Reads the codec.
 *
 * @param options The object.
 * @param out_codec Receives the codec. Written only on success.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for a NULL argument.
 */
GRHEAP_API GRHEAP_Result grheap_options_get_value_codec(
    const GRHEAP_Options * options, GRHEAP_ValueCodec * out_codec);

/**
 * @brief Sets the allocation threshold: how many bytes of objects (counted
 *   as whole cells) may be allocated since the last collection before the
 *   next allocation collects first.
 *
 * After a collection the threshold in force is the larger of this value and
 * the bytes the collection found live, so a heap that is mostly live is not
 * collected on every allocation. Zero means "collect before every
 * allocation" until the first collection finds anything live. Only meaningful
 * while auto-collect is on, and in collected mode.
 *
 * @param options The object.
 * @param bytes The threshold.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for NULL.
 */
GRHEAP_API GRHEAP_Result grheap_options_set_gc_threshold(
    GRHEAP_Options * options, uint64_t bytes);

/**
 * @brief The allocation threshold.
 *
 * @param options The object.
 * @return The threshold; ::GRHEAP_DEFAULT_GC_THRESHOLD for NULL.
 */
GRHEAP_API uint64_t grheap_options_get_gc_threshold(
    const GRHEAP_Options * options);

/**
 * @brief Turns the allocation-triggered collection on or off.
 *
 * Off, the heap collects only when asked (::grheap_collect), when a block or
 * mapping cannot be obtained (once, with a retry), and when a poll asks it to
 * reclaim memory over the context's budget.
 *
 * @param options The object.
 * @param enabled Whether allocation may collect.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for NULL.
 */
GRHEAP_API GRHEAP_Result grheap_options_set_auto_collect(
    GRHEAP_Options * options, bool enabled);

/**
 * @brief Whether allocation may collect.
 *
 * @param options The object.
 * @return The setting; true for NULL.
 */
GRHEAP_API bool grheap_options_get_auto_collect(const GRHEAP_Options * options);

/**
 * @brief Turns GC torture on or off.
 *
 * On, a collected heap collects at every GC point it owns: at the start of
 * every allocation (before the cell is taken) and in every ACT poll. After each
 * sweep every freed cell is poisoned (every byte after its free-list link word
 * set to 0xDD), so a read through a dangling pointer sees poison even though
 * ASan cannot look inside a block. Its purpose is to find a root that is
 * missing: an object kept only in a C variable across an allocation. In an
 * arena nothing is collected, so torture does nothing there.
 *
 * @param options The object.
 * @param enabled Whether to torture.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for NULL.
 */
GRHEAP_API GRHEAP_Result grheap_options_set_torture(
    GRHEAP_Options * options, bool enabled);

/**
 * @brief Whether torture is on.
 *
 * @param options The object.
 * @return The setting; false for NULL.
 */
GRHEAP_API bool grheap_options_get_torture(const GRHEAP_Options * options);

/**
 * @brief Chooses what barrier-verify does.
 *
 * Verify keeps a shadow of the last value written through each
 * `grheap_store*` to a slot of a heap object, and compares it with the slot
 * itself at every collection and in ::grheap_verify. A slot that differs was
 * written without passing a store: the discipline a barrier will rely on.
 * It changes no result of the heap (only its cost).
 *
 * @param options The object.
 * @param mode The mode.
 * @return ::GRHEAP_OK, or ::GRHEAP_ERR_INVALID for NULL or a value outside the
 *   enum (nothing changes).
 */
GRHEAP_API GRHEAP_Result grheap_options_set_verify(
    GRHEAP_Options * options, GRHEAP_Verify mode);

/**
 * @brief The verify mode.
 *
 * @param options The object.
 * @return The mode; ::GRHEAP_VERIFY_OFF for NULL.
 */
GRHEAP_API GRHEAP_Verify grheap_options_get_verify(const GRHEAP_Options * options);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GRHEAP_OPTIONS_H */
