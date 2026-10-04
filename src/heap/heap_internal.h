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
 * @file
 *
 * Private declarations shared by the files under src/heap/: the heap, its
 * spans, the size classes and the helpers that locate an object by address.
 *
 * Layout (AD-11). An object is a 16-byte header `{type, payload size}`, the
 * payload, and at least one pad byte, rounded up to a 16-byte *cell*. The pad
 * byte is what makes a pointer one past the end of the payload land inside
 * the object's own cell and never on the next object. A payload starts 16
 * bytes into its cell, so it is 16-byte aligned.
 *
 * A *span* is one contiguous mapping from the context's page provider, kept in
 * an array sorted by base address so that an address resolves to its span by
 * binary search. There are two kinds. A BLOCK is cut into equal cells of one
 * size class and has an allocated-bit map and a mark-bit map, a free list
 * threaded through its free cells, and a bump index: cells at or above it have
 * never been touched and are still the zero pages the provider gave. An
 * oversized object is a BLOCK with a single cell, so lookup, marking and
 * sweeping treat it like any other. An ARENA chunk is bump allocated, with
 * objects of any size one after another; a bit per 16 bytes marks where each
 * object's cell starts.
 */

#ifndef GHOTI_IO_GRHEAP_SRC_HEAP_HEAP_INTERNAL_H
#define GHOTI_IO_GRHEAP_SRC_HEAP_HEAP_INTERNAL_H

#include <ghoti.io/runtime-heap/macros.h>

#include <ghoti.io/runtime-heap/core.h>
#include <ghoti.io/runtime-heap/heap.h>
#include <ghoti.io/runtime-heap/id.h>
#include <ghoti.io/runtime-heap/options.h>
#include <ghoti.io/runtime-heap/roots.h>
#include <ghoti.io/runtime-heap/store.h>
#include <ghoti.io/runtime-heap/type.h>

#include <ghoti.io/runtime-core/b/budget.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/b/page.h>
#include <ghoti.io/runtime-core/b/poll.h>
#include <ghoti.io/runtime-core/b/roots.h>
#include <ghoti.io/runtime-core/b/snapshot.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Barrier-verify, weak cells and the ID index read and write 64-bit slots as
 * whole words (a raw pointer slot is a uint64_t to them). */
_Static_assert(sizeof(void *) == 8, "weak and verify code assume 8-byte slots");

/** Size classes: 32 to 128 in steps of 16, then four per doubling to 8192.
 *  (A 16-byte cell cannot exist: the header alone is 16 and a pad byte is
 *  required.) */
#define GRHEAP_CLASS_COUNT 31u

/** The smallest block, in bytes. A block is the larger of this and a page. */
#define GRHEAP_BLOCK_MIN 65536u

/** The first object header: what an object is, and how big its payload is. */
typedef struct GRHEAP_Header {
  _Alignas(16) const GRHEAP_Type * type;
  uint64_t size;
} GRHEAP_Header;

_Static_assert(sizeof(GRHEAP_Header) == GRHEAP_HEADER_BYTES,
    "the header must be exactly the 16 bytes the payload offset assumes");

typedef enum {
  GRHEAP_SPAN_BLOCK = 0, ///< Equal cells; also an oversized object alone.
  GRHEAP_SPAN_ARENA      ///< Bump-allocated chunk of an arena heap.
} GRHEAP_SpanKind;

/** One mapping, with the metadata that describes it. */
typedef struct GRHEAP_Span {
  uintptr_t base;    ///< First byte of the mapping.
  size_t size;       ///< Bytes mapped.
  GRHEAP_SpanKind kind;
  bool large;        ///< A BLOCK holding one oversized object.
  unsigned cls;      ///< Size class of a small BLOCK.
  size_t cell;       ///< Cell size in bytes (BLOCK only).
  size_t ncells;     ///< BLOCK: cells that fit. ARENA: 16-byte granules.
  size_t bump;       ///< BLOCK: first never-used cell. ARENA: bytes used.
  size_t live;       ///< Objects allocated and not freed.
  size_t npinned;    ///< BLOCK: cells with a non-zero pin count.
  void * free_list;  ///< BLOCK: first free cell, threaded through the cells.
  uint64_t * alloc;  ///< One bit per cell (BLOCK) or granule (ARENA).
  uint64_t * mark;   ///< BLOCK: one bit per cell. NULL for an ARENA.
  uint8_t * pins;    ///< BLOCK: a pin count per cell, made on the first pin.
  struct GRHEAP_Span * next_avail; ///< The class's list of blocks with room.
  uint64_t bits[];   ///< Storage for `alloc` and `mark`.
} GRHEAP_Span;

/** A handle. A doubly linked list, so that destroying one is O(1). */
struct GRHEAP_Handle {
  struct GRHEAP_Heap * heap;
  void * object;
  struct GRHEAP_Handle * prev;
  struct GRHEAP_Handle * next;
};

/** What a tracer is doing with the slots a `trace` function reports. */
typedef enum {
  GRHEAP_TRACE_MARK = 0, ///< Marking: decode the slot and mark its target.
  GRHEAP_TRACE_VERIFY,   ///< Barrier-verify: compare the slot with its shadow.
  GRHEAP_TRACE_IMAGE,    ///< An image take: turn the slot into an index.
  GRHEAP_TRACE_SHADOW,   ///< After a restore: record the slot as stored.
  GRHEAP_TRACE_RETAIN    ///< A retention query: record the reference found.
} GRHEAP_TraceMode;

struct GRHEAP_Take; /* image.c */
struct GRHEAP_Pending; /* image.c */
struct GRHEAP_Retain; /* retention.c */

/** What a `trace` function is handed. */
struct GRHEAP_Tracer {
  struct GRHEAP_Heap * heap;
  GRHEAP_TraceMode mode;
  const GRHEAP_Type * type; ///< VERIFY: the object being walked.
  void * payload;           ///< VERIFY: its payload.
  uint64_t found;           ///< VERIFY: violations found by this walk.
  struct GRHEAP_Take * take; ///< IMAGE: the take in progress.
  unsigned char * base;     ///< IMAGE: the copy of the payload being walked.
  size_t size;              ///< IMAGE: its size.
  struct GRHEAP_Retain * retain; ///< RETAIN: the query in progress. `payload`
                            ///< is the holder being expanded.
};

/** One shadow slot: where, and the last value stored through a grheap_store. */
typedef struct GRHEAP_ShadowEntry {
  uintptr_t slot;  ///< Zero marks an empty entry.
  uint64_t value;  ///< Never zero (a zero value is an absent entry).
} GRHEAP_ShadowEntry;

/** A registered weak cell or ephemeron. */
typedef struct GRHEAP_WeakEntry {
  void * object;   ///< Its payload.
  bool pending;    ///< A slot was cleared; `cleared` is due after the sweep.
} GRHEAP_WeakEntry;

/** A stable ID and its object. The array is sorted by `id`. */
typedef struct GRHEAP_IdEntry {
  uint64_t id;
  void * object;
} GRHEAP_IdEntry;

/** The options object. */
struct GRHEAP_Options {
  const GRHEAP_Allocator * allocator;
  GRHEAP_Mode mode;
  GRHEAP_ValueCodec codec;
  uint64_t threshold;
  bool auto_collect;
  bool torture;
  GRHEAP_Verify verify;
};

/** The heap. */
struct GRHEAP_Heap {
  GRCORE_Context * context;
  const GRCORE_Allocator * allocator; ///< The context's counting allocator.
  const GRCORE_PageProvider * pages;  ///< The context's counting provider.
  size_t block_size;                  ///< max(64 KiB, a page).
  GRHEAP_Mode mode;
  GRHEAP_ValueCodec codec;
  uint64_t threshold;                 ///< The option.
  bool auto_collect;

  GRHEAP_Span ** spans;               ///< Sorted by base address.
  size_t nspans;
  size_t spans_cap;
  GRHEAP_Span * avail[GRHEAP_CLASS_COUNT]; ///< Blocks with room, per class.
  GRHEAP_Span * arena_cur;            ///< The chunk being bump allocated.

  void *** roots;                     ///< Registered root slots.
  size_t nroots;
  size_t roots_cap;
  struct GRHEAP_Handle * handles;     ///< Head of the handle list.

  void ** mark_stack;                 ///< Payloads waiting to be traced.
  size_t mark_top;
  size_t mark_cap;
  bool mark_overflow;                 ///< A push was dropped; rescan.

  bool torture;                       ///< Collect at every GC point.
  GRHEAP_Verify verify;               ///< Barrier-verify mode.
  GRHEAP_ShadowEntry * shadow;        ///< Open-addressing table, or NULL.
  size_t shadow_cap;                  ///< A power of two, or 0.
  size_t shadow_count;
  bool shadow_lost;                   ///< An insert failed; no comparison holds.

  GRHEAP_WeakEntry * weak;            ///< Registered weak cells, ephemerons.
  size_t nweak;
  size_t weak_cap;

  GRHEAP_IdEntry * ids;               ///< Stable IDs, sorted by ID.
  size_t nids;
  size_t ids_cap;
  size_t * id_index;                  ///< Object -> position + 1; open addressing.
  size_t id_index_cap;                ///< A power of two, or 0.
  uint64_t next_id;                   ///< The last ID issued.

  struct GRHEAP_Pending * pending;    ///< A restore between APPLY and COMMIT.

  unsigned busy;                      ///< Inside a collection, trace or destroy.
  uint64_t since_collect;             ///< Cell bytes allocated since the last.
  uint64_t next_threshold;            ///< The threshold now in force.
  GRHEAP_Stats stats;
};

/** The heap's key (cardinality one, ACT phase). Its address is the identity. */
GRHEAP_INTERNAL_API extern const GRCORE_Key grheap_key;

/* ---- bit maps --------------------------------------------------------- */

static inline bool grheap_bit_get(const uint64_t * map, size_t i) {
  return (map[i >> 6] >> (i & 63u)) & 1u;
}

static inline void grheap_bit_set(uint64_t * map, size_t i) {
  map[i >> 6] |= UINT64_C(1) << (i & 63u);
}

static inline void grheap_bit_clear(uint64_t * map, size_t i) {
  map[i >> 6] &= ~(UINT64_C(1) << (i & 63u));
}

static inline size_t grheap_bit_words(size_t bits) {
  return (bits + 63u) / 64u;
}

/* ---- heap state ------------------------------------------------------- */

/** Whether the calling thread may use the heap. */
static inline bool grheap_usable(const GRHEAP_Heap * heap) {
  return heap != NULL && grcore_context_is_owner(heap->context);
}

/** The environment's default for torture (`GRHEAP_TORTURE=1`). */
GRHEAP_INTERNAL_API bool grheap_env_torture(void);

/** The environment's default for verify (`GRHEAP_VERIFY=1` or `count`). */
GRHEAP_INTERNAL_API GRHEAP_Verify grheap_env_verify(void);

/** Maps a runtime-core result onto this library's (same numbering, no GUEST). */
GRHEAP_INTERNAL_API GRHEAP_Result grheap_from_core(GRCORE_Result result);

/** The result for an allocation that failed: LIMIT if the context's budget
 *  refused anything since `refusals_before`, else OOM. */
GRHEAP_INTERNAL_API GRHEAP_Result grheap_failure(
    const GRHEAP_Heap * heap, uint64_t refusals_before);

/* ---- span.c ----------------------------------------------------------- */

/** The cell size for a payload of `payload` bytes; false if it overflows. */
GRHEAP_INTERNAL_API bool grheap_cell_size(size_t payload, size_t * out_cell);

/** The size class of a cell of at most GRHEAP_SMALL_CELL_MAX bytes: its index,
 *  and the class's own cell size. */
GRHEAP_INTERNAL_API unsigned grheap_class_of(size_t cell, size_t * out_class_cell);

/** The span containing `addr`, or NULL. */
GRHEAP_INTERNAL_API GRHEAP_Span * grheap_span_find(
    const GRHEAP_Heap * heap, uintptr_t addr);

/**
 * Makes a span: its metadata through the allocator, its pages from the
 * provider, and an entry in the sorted array (reserved first, so that the
 * insert cannot fail). A failure leaves no span and no mapping behind; the
 * sorted array may stay grown, which is memory the heap keeps for its next span.
 *
 * @param cell For a BLOCK, the cell size; for a large one, the object's cell.
 */
GRHEAP_INTERNAL_API GRHEAP_Span * grheap_span_create(
    GRHEAP_Heap * heap, GRHEAP_SpanKind kind, size_t cell, bool large);

/** Unmaps and frees a span. The caller removes it from the array. */
GRHEAP_INTERNAL_API void grheap_span_free(GRHEAP_Heap * heap, GRHEAP_Span * span);

/**
 * Resolves an address to an allocated cell.
 *
 * @param exact Require the address to be the payload start; otherwise any
 *   address inside the cell counts (a BLOCK only: an arena is never scanned
 *   conservatively).
 */
GRHEAP_INTERNAL_API bool grheap_locate(const GRHEAP_Heap * heap, uintptr_t addr,
    bool exact, GRHEAP_Span ** out_span, size_t * out_index);

/** The payload start of cell `index` of `span`. */
static inline void * grheap_payload(const GRHEAP_Span * span, size_t index) {
  size_t stride = span->kind == GRHEAP_SPAN_ARENA ? 16u : span->cell;
  return (void *)(span->base + index * stride + GRHEAP_HEADER_BYTES);
}

/** The header of a payload. */
static inline GRHEAP_Header * grheap_header(void * payload) {
  return (GRHEAP_Header *)((unsigned char *)payload - GRHEAP_HEADER_BYTES);
}

/** The cell size of an object with `size` payload bytes. */
static inline size_t grheap_cell_of_size(uint64_t size) {
  return ((size_t)size + GRHEAP_HEADER_BYTES + 1u + 15u) & ~(size_t)15u;
}

/* ---- alloc.c / arena.c ------------------------------------------------ */

/** Takes a cell for an object (collected mode): from a free list, the bump
 *  index, a new block, or a large mapping. NULL if no memory could be had. */
GRHEAP_INTERNAL_API void * grheap_take_cell(GRHEAP_Heap * heap, size_t cell);

/** Takes a cell in arena mode. */
GRHEAP_INTERNAL_API void * grheap_arena_take(GRHEAP_Heap * heap, size_t cell);

/** Runs the `destroy` hook of every live object, frees nothing. */
GRHEAP_INTERNAL_API void grheap_run_all_destroys(GRHEAP_Heap * heap);

/** Unmaps every span and empties the array. The hooks have run already. */
GRHEAP_INTERNAL_API void grheap_release_all_spans(GRHEAP_Heap * heap);

/* ---- mark.c / sweep.c / collect.c ------------------------------------- */

/** Marks everything reachable from the roots. Never fails. */
GRHEAP_INTERNAL_API void grheap_mark(GRHEAP_Heap * heap);

/** Frees what was not marked, rebuilds the free lists, returns empty spans. */
GRHEAP_INTERNAL_API void grheap_sweep(GRHEAP_Heap * heap);

/** Releases the mark stack's memory. */
GRHEAP_INTERNAL_API void grheap_mark_stack_free(GRHEAP_Heap * heap);

/** Runs one collection. The caller has checked ownership and `busy`. */
GRHEAP_INTERNAL_API void grheap_collect_now(GRHEAP_Heap * heap);

/** Decodes a word with the heap's codec: false if the tag test fails. */
GRHEAP_INTERNAL_API bool grheap_decode(
    const GRHEAP_Heap * heap, uint64_t word, uint64_t * out_address);

/** Marks the object at (span, index) and everything newly reachable from it,
 *  draining the mark stack. Counts nothing: the caller has validated it. */
GRHEAP_INTERNAL_API void grheap_mark_extra(
    GRHEAP_Heap * heap, GRHEAP_Span * span, size_t index);

/** Marks every pinned object. */
GRHEAP_INTERNAL_API void grheap_mark_pinned(GRHEAP_Heap * heap);

/** Frees every handle and the root array. */
GRHEAP_INTERNAL_API void grheap_roots_free(GRHEAP_Heap * heap);

/** Clears every handle's object. */
GRHEAP_INTERNAL_API void grheap_roots_clear_for_reset(GRHEAP_Heap * heap);

/* ---- digest.c --------------------------------------------------------- */

/** A 64-bit digest of the heap's bookkeeping (bits, lists, counters), for
 *  tests that a read-only operation changed none of it. Test-only: it is not
 *  in the shipped library (see TEST_ONLY_SOURCES in the Makefile). */
GRHEAP_INTERNAL_API uint64_t grheap_state_digest(const GRHEAP_Heap * heap);

/* ---- retention.c ------------------------------------------------------ */

/** A RETAIN-mode tracer reports a slot (encoded: a codec word; else a raw
 *  pointer). Changes no heap state. */
GRHEAP_INTERNAL_API void grheap_retention_trace(
    GRHEAP_Tracer * tracer, void * slot, bool encoded);

/* ---- verify.c --------------------------------------------------------- */

/** Calls `fn(user, payload)` for every live object, in an unspecified order.
 *  `fn` must not allocate or free in the heap. */
GRHEAP_INTERNAL_API void grheap_each_object(GRHEAP_Heap * heap,
    void (*fn)(void * user, void * payload), void * user);

/** The live object whose cell contains `addr`: its payload start and the first
 *  address past its cell. False if `addr` lies in no live object's cell. */
GRHEAP_INTERNAL_API bool grheap_object_containing(const GRHEAP_Heap * heap,
    uintptr_t addr, uintptr_t * out_payload, uintptr_t * out_end);

/** Records `value` as the last thing stored to `slot` (zero forgets it). If the
 *  table cannot grow, sets `shadow_lost`. */
GRHEAP_INTERNAL_API void grheap_shadow_set(
    GRHEAP_Heap * heap, uintptr_t slot, uint64_t value);

/** Forgets every shadow entry whose slot lies in [start, start + bytes). */
GRHEAP_INTERNAL_API void grheap_shadow_forget_range(
    GRHEAP_Heap * heap, uintptr_t start, size_t bytes);

/** Forgets everything and clears `shadow_lost`. */
GRHEAP_INTERNAL_API void grheap_shadow_clear(GRHEAP_Heap * heap);

/** Frees the table. */
GRHEAP_INTERNAL_API void grheap_shadow_free(GRHEAP_Heap * heap);

/** After a store of `count` slots at `slot`: checks the holder, records the
 *  new values. */
GRHEAP_INTERNAL_API void grheap_verify_store(GRHEAP_Heap * heap,
    const void * holder, const void * slot, size_t count);

/** Compares every traced and weak slot of every live object with its shadow.
 *  Returns the violations found. The caller holds `busy`. */
GRHEAP_INTERNAL_API uint64_t grheap_verify_walk(GRHEAP_Heap * heap);

/** A tracer in VERIFY mode reports one slot. */
GRHEAP_INTERNAL_API void grheap_verify_slot(
    GRHEAP_Tracer * tracer, const void * slot);

/* ---- weak.c ----------------------------------------------------------- */

/** Whether `type`'s weak spec fits in a payload of `bytes` bytes. */
GRHEAP_INTERNAL_API bool grheap_weak_spec_fits(
    const GRHEAP_Type * type, size_t bytes);

/** Makes room for one more registration, so the registration cannot fail. */
GRHEAP_INTERNAL_API bool grheap_weak_reserve(GRHEAP_Heap * heap);

/** Registers a new weak object. Room was reserved. */
GRHEAP_INTERNAL_API void grheap_weak_register(GRHEAP_Heap * heap, void * payload);

/** After marking: the ephemeron fixpoint, then clearing, then dropping the dead
 *  from the registry. Allocates nothing. */
GRHEAP_INTERNAL_API void grheap_weak_process(GRHEAP_Heap * heap);

/** After the sweep: calls `cleared` for every cell that is due one. */
GRHEAP_INTERNAL_API void grheap_weak_run_cleared(GRHEAP_Heap * heap);

/** The weak and ephemeron slots of an object, for verify: calls
 *  `fn(user, slot_address)` for each. */
GRHEAP_INTERNAL_API void grheap_weak_each_slot(const GRHEAP_Type * type,
    void * payload, void (*fn)(void * user, const void * slot), void * user);

/** Empties the registry (arena reset). */
GRHEAP_INTERNAL_API void grheap_weak_free(GRHEAP_Heap * heap);

/* ---- image.c ---------------------------------------------------------- */

/** An IMAGE-mode tracer reports a slot (encoded: a codec word; else a raw
 *  pointer). */
GRHEAP_INTERNAL_API void grheap_image_trace(
    GRHEAP_Tracer * tracer, void * slot, bool encoded);

/** Frees a restore that was never settled (the heap's destruction). */
GRHEAP_INTERNAL_API void grheap_pending_free(GRHEAP_Heap * heap);

/** The heap key's snapshot hooks (core's types; the heap has one blob). */
GRHEAP_INTERNAL_API GRCORE_Result grheap_key_snapshot(
    GRCORE_Context * context, void * value, GRCORE_SnapshotWriter * writer);
GRHEAP_INTERNAL_API GRCORE_Result grheap_key_restore(GRCORE_Context * context,
    void * value, GRCORE_SnapshotReader * reader, void * env,
    GRCORE_RestoreMode mode);
GRHEAP_INTERNAL_API GRCORE_Result grheap_key_settle(GRCORE_Context * context,
    void * value, void * env, GRCORE_SettleMode mode);

/* ---- id.c ------------------------------------------------------------- */

/** The stable ID of `object` if it has one, else 0. Never issues one. */
GRHEAP_INTERNAL_API uint64_t grheap_id_lookup(
    const GRHEAP_Heap * heap, const void * object);

/** Makes room for `total` entries in all (a restore's count), or false with
 *  nothing changed. */
GRHEAP_INTERNAL_API bool grheap_ids_reserve_total(
    GRHEAP_Heap * heap, size_t total);

/** Adopts `count` entries a restore wrote at `heap->ids[0..count)` (room was
 *  reserved), in any order: sorts them, rebuilds the index, sets the next ID to
 *  issue. */
GRHEAP_INTERNAL_API void grheap_ids_adopt(
    GRHEAP_Heap * heap, size_t count, uint64_t last_id);

/** Forgets what a restore adopted, and the numbering with it. */
GRHEAP_INTERNAL_API void grheap_ids_unadopt(GRHEAP_Heap * heap);

/** After marking: forgets the ID of every unmarked object. */
GRHEAP_INTERNAL_API void grheap_ids_drop_dead(GRHEAP_Heap * heap);

/** Forgets every ID (arena reset); numbering carries on. */
GRHEAP_INTERNAL_API void grheap_ids_clear(GRHEAP_Heap * heap);

/** Frees the tables. */
GRHEAP_INTERNAL_API void grheap_ids_free(GRHEAP_Heap * heap);

#endif /* GHOTI_IO_GRHEAP_SRC_HEAP_HEAP_INTERNAL_H */
