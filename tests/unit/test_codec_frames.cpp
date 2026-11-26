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
 * Two engines with different value words running guest frames on one heap.
 * The frames are one root source (runtime-core's), so the codec comes from the
 * engine of each frame and not from the source. "P" keeps a reference as the
 * address under a 16-bit tag; "Q" keeps it shifted left by four above a tag,
 * so that no word of one engine is a word of the other for the same cell. The
 * heap's own codec is the identity, which reads neither: a frame slot decoded
 * by the wrong codec names no object and is counted in `invalid_pointers`.
 *
 * Every reference a test holds is in a frame slot, so the tests are correct
 * under torture, verify, relocation and mark-step torture, which the worlds
 * here turn on whatever the environment says.
 */

#include "../test_helpers.h"

#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/a/unwind.h>
#include <ghoti.io/runtime-core/b/handles.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

const GRHEAP_ValueCodec kP = {
    UINT64_C(0xFFFF) << 48, UINT64_C(0xFFFC) << 48, UINT64_C(0x0000FFFFFFFFFFFF), 0, 0};
const GRHEAP_ValueCodec kQ = {0xF, 0x5, ~UINT64_C(0xF), 4, 0};
uint64_t encP(const void * p) { return (UINT64_C(0xFFFC) << 48) | addr(p); }
uint64_t encQ(const void * p) { return (addr(p) << 4) | 5u; }
// An immediate of each: never a reference in its own engine.
uint64_t immP(uint64_t n) { return (UINT64_C(0x3FF0) << 48) | n; }
uint64_t immQ(uint64_t n) { return (n << 4) | 6u; }

struct Cell {
  uint64_t ref; // a word in the cell's engine's encoding
  uint64_t id;
};
void cell_trace(GRHEAP_Tracer * t, void * payload) {
  grheap_trace_word(t, &static_cast<Cell *>(payload)->ref);
}
const GRHEAP_Type kCellP = GRHEAP_TYPE_INIT_CODEC(
    "cell-p", sizeof(Cell), cell_trace, nullptr, nullptr, nullptr, nullptr, &kP);
const GRHEAP_Type kCellQ = GRHEAP_TYPE_INIT_CODEC(
    "cell-q", sizeof(Cell), cell_trace, nullptr, nullptr, nullptr, nullptr, &kQ);

GRCORE_SlotKind all_values(const GRCORE_AbstractFrame *, size_t) {
  return GRCORE_SLOT_VALUE;
}
const GRCORE_EngineDescriptor kEngineP = GRCORE_ENGINE_DESCRIPTOR_INIT_CODEC(
    &kP, "engine-p", all_values, nullptr, nullptr,
    GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr, nullptr, nullptr);
const GRCORE_EngineDescriptor kEngineQ = GRCORE_ENGINE_DESCRIPTOR_INIT_CODEC(
    &kQ, "engine-q", all_values, nullptr, nullptr,
    GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr, nullptr, nullptr);
// The controls: Q naming no codec, and Q naming another one.
const GRCORE_EngineDescriptor kEngineQForgot = GRCORE_ENGINE_DESCRIPTOR_INIT(
    "engine-q", all_values, nullptr, nullptr,
    GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr, nullptr, nullptr);
const GRHEAP_ValueCodec kQother = {0xF, 0x9, ~UINT64_C(0xF), 4, 0};
const GRCORE_EngineDescriptor kEngineQOther = GRCORE_ENGINE_DESCRIPTOR_INIT_CODEC(
    &kQother, "engine-q", all_values, nullptr, nullptr,
    GRCORE_ScopeInterface{nullptr, nullptr, nullptr},
    GRCORE_ConservativeDecoder{0, 0, 0}, nullptr, nullptr, nullptr, nullptr);

/* Every instrument the heap has, whatever the environment says. */
WorldConfig hostile(int mark_torture = GRHEAP_MARK_TORTURE_COUNT) {
  WorldConfig c;
  c.torture = 1;
  c.verify = 1;
  c.mark_torture = mark_torture;
  c.relocate = 1; // read only by a RELOCATE=yes build
  return c;
}

/* One world, two engines, interleaved frames of three slots each: slot 0 the
 * frame's own cell, slot 1 an immediate, slot 2 a second cell reached through
 * the first. */
struct Frames {
  HeapWorld w;
  GRCORE_EngineId p = 0, q = 0;
  GRCORE_Stack * stack = nullptr;
  std::vector<GRCORE_FrameRef> refs;
  std::vector<bool> is_q;
  explicit Frames(const WorldConfig & config = hostile(),
      const GRCORE_EngineDescriptor * second = &kEngineQ)
      : w(config) {
    EXPECT_EQ(grcore_engine_register(w.ctx, &kEngineP, &p), GRCORE_OK);
    EXPECT_EQ(grcore_engine_register(w.ctx, second, &q), GRCORE_OK);
    stack = grcore_context_stack(w.ctx);
  }
  ~Frames() { (void)grcore_unwind_all(stack, nullptr); }
  size_t push(bool use_q) {
    GRCORE_FrameRef f = {0};
    EXPECT_EQ(grcore_stack_push(stack, use_q ? q : p, 3, &f), GRCORE_OK);
    refs.push_back(f);
    is_q.push_back(use_q);
    return refs.size() - 1;
  }
  uint64_t slot(size_t frame, size_t i) const {
    uint64_t v = 0;
    EXPECT_EQ(grcore_stack_slot_get(stack, refs[frame], i, &v), GRCORE_OK);
    return v;
  }
  void set(size_t frame, size_t i, uint64_t v) {
    EXPECT_EQ(grcore_stack_slot_set(stack, refs[frame], i, v), GRCORE_OK);
  }
  void * alloc(bool use_q, uint64_t id) {
    void * obj = nullptr;
    EXPECT_EQ(grheap_alloc(w.heap, use_q ? &kCellQ : &kCellP, &obj), GRHEAP_OK);
    static_cast<Cell *>(obj)->id = id;
    return obj;
  }
  Cell * decode(bool use_q, uint64_t word) const {
    const GRHEAP_ValueCodec & c = use_q ? kQ : kP;
    if ((word & c.tag_mask) != c.tag_value) {
      return nullptr;
    }
    auto * obj = reinterpret_cast<Cell *>(((word & c.mask) >> c.shift) + c.base);
    return grheap_contains(w.heap, obj) ? obj : nullptr;
  }
  /* A frame of each kind, each holding a pair of cells with ids 10n and
   * 10n + 1. Every allocation is a collection under torture. */
  void build(size_t frames) {
    for (size_t n = 0; n < frames; n++) {
      bool use_q = n % 2 == 1;
      size_t f = push(use_q);
      void * first = alloc(use_q, 10 * n);
      set(f, 0, use_q ? encQ(first) : encP(first));
      set(f, 1, use_q ? immQ(n) : immP(n));
      void * second = alloc(use_q, 10 * n + 1);
      set(f, 2, use_q ? encQ(second) : encP(second));
      // Re-read both: the allocation may have moved the first.
      Cell * a = decode(use_q, slot(f, 0));
      ASSERT_NE(a, nullptr) << "frame " << n;
      ASSERT_EQ(grheap_store_word(w.heap, a, &a->ref, slot(f, 2)), GRHEAP_OK);
    }
  }
  void check(size_t frames) {
    for (size_t n = 0; n < frames; n++) {
      bool use_q = n % 2 == 1;
      Cell * a = decode(use_q, slot(n, 0));
      Cell * b = decode(use_q, slot(n, 2));
      ASSERT_NE(a, nullptr) << "frame " << n;
      ASSERT_NE(b, nullptr) << "frame " << n;
      EXPECT_EQ(a->id, 10 * n) << "frame " << n;
      EXPECT_EQ(b->id, 10 * n + 1) << "frame " << n;
      EXPECT_EQ(a->ref, slot(n, 2)) << "the field and the slot name one cell";
      EXPECT_EQ(slot(n, 1), use_q ? immQ(n) : immP(n)) << "an immediate is not rewritten";
    }
  }
};

constexpr size_t kFrames = 12;

} // namespace

TEST(CodecFrames, TwoEnginesRunFramesOnOneHeapUnderEveryInstrument) {
  Frames f;
  f.build(kFrames);
  for (int round = 0; round < 4; round++) {
    ASSERT_EQ(grheap_gc_point(f.w.heap, GRHEAP_GC_POINT_FRAME_PUSH), GRHEAP_OK);
    ASSERT_EQ(grheap_collect(f.w.heap), GRHEAP_OK);
  }
  f.check(kFrames);
  GRHEAP_Stats s = f.w.stats();
  EXPECT_EQ(s.invalid_pointers, 0u) << "a frame slot was decoded by the wrong codec";
  EXPECT_EQ(s.live_objects, 2 * kFrames);
  EXPECT_EQ(s.mark_violations, 0u);
  EXPECT_GT(s.mark_cycles, 0u) << "control: the interleaved mark ran";
#ifdef GRHEAP_RELOCATE
  GRHEAP_RelocationStats moves = {};
  ASSERT_EQ(grheap_relocation_stats(f.w.heap, &moves), GRHEAP_OK);
  EXPECT_GT(moves.moved, 0u) << "control: objects moved under the frames";
#endif
  // A popped frame's cells die, and only they.
  ASSERT_EQ(grcore_stack_pop(f.stack), GRCORE_OK);
  ASSERT_EQ(grheap_collect(f.w.heap), GRHEAP_OK);
  EXPECT_EQ(f.w.stats().live_objects, 2 * kFrames - 2);
  f.refs.pop_back();
}

TEST(CodecFrames, AControlWithTheSecondEngineNamingNoCodecLosesItsCells) {
  WorldConfig c = steady();
  Frames f(c, &kEngineQForgot);
  size_t q = f.push(true);
  void * cell = f.alloc(true, 1);
  f.set(q, 0, encQ(cell));
  ASSERT_EQ(grheap_collect(f.w.heap), GRHEAP_OK);
  EXPECT_EQ(f.w.stats().live_objects, 0u) << "the identity cannot read a Q word";
}

TEST(CodecFrames, ARetentionPathStartsAtAFrameSlotOfEitherEngine) {
  Frames f(steady());
  f.build(2);
  for (size_t n = 0; n < 2; n++) {
    Cell * b = f.decode(n == 1, f.slot(n, 2));
    ASSERT_NE(b, nullptr);
    GRHEAP_Retention * path = nullptr;
    ASSERT_EQ(grheap_retention_path(f.w.heap, b, nullptr, &path), GRHEAP_OK);
    EXPECT_TRUE(grheap_retention_retained(path)) << "engine " << n;
    grheap_retention_release(path);
  }
}

TEST(CodecFrames, AHandleTableEntryIsRelocatedByTheCodecItWasPinnedWith) {
  Frames f;
  GRCORE_HandleTable * table = grcore_group_handles(f.w.group);
  GRCORE_HandleHolder holder = GRCORE_HANDLE_HOLDER_INIT;
  ASSERT_EQ(grcore_handles_enroll(table, f.w.ctx, &holder), GRCORE_OK);
  size_t q = f.push(true);
  f.set(q, 0, encQ(f.alloc(true, 7)));
  GRCORE_HandleToken token = GRCORE_HANDLE_TOKEN_INIT;
  ASSERT_EQ(grcore_handles_pin_codec(table, f.w.ctx, holder, f.slot(q, 0), &kQ, &token),
      GRCORE_OK);
  // The entry is now the only thing that names the cell.
  f.set(q, 0, immQ(0));
  for (int round = 0; round < 3; round++) {
    ASSERT_EQ(grheap_collect(f.w.heap), GRHEAP_OK);
  }
  uint64_t word = 0;
  ASSERT_EQ(grcore_handles_resolve(table, token, f.w.ctx, &word), GRCORE_HANDLE_LIVE);
  Cell * cell = f.decode(true, word);
  ASSERT_NE(cell, nullptr);
  EXPECT_EQ(cell->id, 7u);
  EXPECT_EQ(f.w.stats().invalid_pointers, 0u);
  EXPECT_EQ(grcore_handles_release(table, token, f.w.ctx), GRCORE_HANDLE_LIVE);
}

namespace {

const GRHEAP_Type * resolve_frame_cells(void *, const char * name) {
  if (std::strcmp(name, "cell-p") == 0) {
    return &kCellP;
  }
  return std::strcmp(name, "cell-q") == 0 ? &kCellQ : nullptr;
}

GRHEAP_Result instantiate(Frames & dst, const GRHEAP_Image * image) {
  GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT(nullptr, nullptr);
  env.resolve = resolve_frame_cells;
  return grheap_image_instantiate(dst.w.heap, image, &env);
}

} // namespace

TEST(CodecFrames, AnImageRebuildsEachFramesWordsAndAnotherEncodingIsRefused) {
  Frames src(steady());
  src.build(4);
  GRHEAP_Image * image = nullptr;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &image), GRHEAP_OK);
  EXPECT_EQ(grheap_image_object_count(image), 8u);

  Frames dst(steady());
  for (size_t n = 0; n < 4; n++) {
    dst.push(n % 2 == 1);
  }
  ASSERT_EQ(instantiate(dst, image), GRHEAP_OK);
  // The immediates are the frames' own, which the heap's image does not carry.
  for (size_t n = 0; n < 4; n++) {
    dst.set(n, 1, n % 2 == 1 ? immQ(n) : immP(n));
  }
  dst.check(4);
  ASSERT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
  EXPECT_EQ(dst.w.stats().live_objects, 8u);
  EXPECT_EQ(dst.w.stats().invalid_pointers, 0u);

  Frames other(steady(), &kEngineQOther);
  for (size_t n = 0; n < 4; n++) {
    other.push(n % 2 == 1);
  }
  EXPECT_EQ(instantiate(other, image), GRHEAP_ERR_INVALID);
  EXPECT_EQ(other.w.stats().live_objects, 0u);
  grheap_image_release(image);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
