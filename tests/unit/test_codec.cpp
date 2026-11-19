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
 * A value codec per object type and per root source: two engines whose value
 * words differ share one heap. "X" is a NaN-boxed word (a reference is the
 * address under a 16-bit tag), "Y" a low-bit tagged one (a reference is the
 * address with bit 0 set). Neither is the heap's own codec, which stays the
 * identity throughout, so a word that is decoded by the wrong codec is
 * visible: it names no object and is counted in `invalid_pointers`.
 *
 * Every reference a test holds is in a registered root or a root source's
 * slot, so the tests are correct under torture, barrier-verify and relocation.
 */

#include "../test_helpers.h"

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

const GRHEAP_ValueCodec kX = {
    UINT64_C(0xFFFF) << 48, UINT64_C(0xFFFC) << 48, UINT64_C(0x0000FFFFFFFFFFFF), 0, 0};
const GRHEAP_ValueCodec kY = {7, 1, ~UINT64_C(7), 0, 0};
// X again, under another tag: a destination that disagrees about the encoding.
const GRHEAP_ValueCodec kXother = {
    UINT64_C(0xFFFF) << 48, UINT64_C(0xFFFD) << 48, UINT64_C(0x0000FFFFFFFFFFFF), 0, 0};

uint64_t encX(const void * p) { return (UINT64_C(0xFFFC) << 48) | addr(p); }
uint64_t encY(const void * p) { return addr(p) | 1u; }
// Immediates. The "trap" ones are words the *other* engine's codec would take
// for a reference: a heap that used one codec for both would count them as
// invalid pointers.
uint64_t immX(uint64_t n) { return (UINT64_C(0x3FF0) << 48) | (n << 8); }
uint64_t immX_looks_like_y(uint64_t n) { return immX(n) | 1u; }
uint64_t immY_looks_like_x(uint64_t n) { return (UINT64_C(0xFFFC) << 48) | (n << 3) | 2u; }

struct Cell {
  uint64_t a;
  uint64_t b;
  uint64_t id;
};

void cell_trace(GRHEAP_Tracer * t, void * payload) {
  auto * c = static_cast<Cell *>(payload);
  grheap_trace_word(t, &c->a);
  grheap_trace_word(t, &c->b);
}

const GRHEAP_Type kCellX = GRHEAP_TYPE_INIT_CODEC(
    "x", sizeof(Cell), cell_trace, nullptr, nullptr, nullptr, nullptr, &kX);
const GRHEAP_Type kCellY = GRHEAP_TYPE_INIT_CODEC(
    "y", sizeof(Cell), cell_trace, nullptr, nullptr, nullptr, nullptr, &kY);
// The same layout as Y, but naming no codec: decoded by the heap's identity.
const GRHEAP_Type kCellYForgot = GRHEAP_TYPE_INIT(
    "y", sizeof(Cell), cell_trace, nullptr, nullptr, nullptr, nullptr);
// A destination's type "x" that disagrees about the encoding.
const GRHEAP_Type kCellXother = GRHEAP_TYPE_INIT_CODEC(
    "x", sizeof(Cell), cell_trace, nullptr, nullptr, nullptr, nullptr, &kXother);

Cell * make(GRHEAP_Heap * heap, const GRHEAP_Type * type, uint64_t id) {
  void * p = nullptr;
  EXPECT_EQ(grheap_alloc(heap, type, &p), GRHEAP_OK);
  auto * c = static_cast<Cell *>(p);
  if (c != nullptr) {
    c->id = id;
  }
  return c;
}

// The words are stored through the heap's entry, as every engine does.
void put(GRHEAP_Heap * heap, Cell * holder, uint64_t Cell::*field, uint64_t word) {
  ASSERT_EQ(grheap_store_word(heap, holder, &(holder->*field), word), GRHEAP_OK);
}

// ---- root sources, one per engine ---------------------------------------

struct Slots {
  std::vector<uint64_t> words;
};
void slots_enumerate(
    GRCORE_Context *, void * value, const GRCORE_RootVisitor * visitor) {
  for (uint64_t & w : static_cast<Slots *>(value)->words) {
    visitor->slot(visitor->user, &w);
  }
}
const GRCORE_RootSource kSourceX = GRCORE_ROOT_SOURCE_INIT_CODEC("engine-x", slots_enumerate, &kX);
const GRCORE_RootSource kSourceY = GRCORE_ROOT_SOURCE_INIT_CODEC("engine-y", slots_enumerate, &kY);
// A source that names none: the heap's identity decodes it.
const GRCORE_RootSource kSourcePlain = GRCORE_ROOT_SOURCE_INIT("plain", slots_enumerate);
// A destination whose source for the Y engine disagrees about the encoding.
const GRCORE_RootSource kSourceYother =
    GRCORE_ROOT_SOURCE_INIT_CODEC("engine-y", slots_enumerate, &kXother);

class CodecTest : public ::testing::Test {};

// What the cells say about themselves: the heap holds the cell, and its id is
// the one it was made with.
bool holds(HeapWorld & w, uint64_t word, const GRHEAP_ValueCodec & codec, uint64_t id) {
  if ((word & codec.tag_mask) != codec.tag_value) {
    return false;
  }
  uint64_t a = ((word & codec.mask) >> codec.shift) + codec.base;
  return grheap_contains(w.heap, reinterpret_cast<const void *>(a)) &&
      reinterpret_cast<const Cell *>(a)->id == id;
}

}  // namespace

TEST_F(CodecTest, TwoEnginesWithDifferentWordsShareOneHeapAndEachWordIsReadByItsOwnTypesCodec) {
  HeapWorld w;  // the heap's own codec is the identity
  Slots rx;
  rx.words.assign(1, 0);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceX, &rx), GRCORE_OK);

  Cell * x1 = make(w.heap, &kCellX, 1);
  rx.words[0] = encX(x1);
  Cell * y1 = make(w.heap, &kCellY, 2);
  // Re-read x1 through the root: the allocation above may have moved it.
  x1 = reinterpret_cast<Cell *>(rx.words[0] & kX.mask);
  put(w.heap, x1, &Cell::a, encX(y1));                      // an X word naming a Y cell
  put(w.heap, x1, &Cell::b, immX_looks_like_y(7));          // and an X immediate Y would take
  put(w.heap, y1, &Cell::a, encY(x1));                      // a Y word naming the X cell: a cycle
  put(w.heap, y1, &Cell::b, immY_looks_like_x(9));          // and a Y immediate X would take
  Cell * dead_x = make(w.heap, &kCellX, 3);
  Cell * dead_y = make(w.heap, &kCellY, 4);
  (void)dead_x;
  (void)dead_y;

  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  GRHEAP_Stats s = w.stats();
  EXPECT_EQ(s.invalid_pointers, 0u) << "a word was decoded by the other engine's codec";
  EXPECT_EQ(s.live_objects, 2u) << "the pair is live through x1's X word; the other pair is not";
  x1 = reinterpret_cast<Cell *>(rx.words[0] & kX.mask);
  EXPECT_EQ(x1->id, 1u);
  ASSERT_TRUE(holds(w, x1->a, kX, 2));
  Cell * y = reinterpret_cast<Cell *>(x1->a & kX.mask);
  EXPECT_TRUE(holds(w, y->a, kY, 1)) << "the Y word still names the X cell";
  EXPECT_EQ(x1->b, immX_looks_like_y(7)) << "an immediate is never rewritten";
  EXPECT_EQ(y->b, immY_looks_like_x(9));
}

TEST_F(CodecTest, AControlWithNoCodecOnTheYTypeShowsTheTestSeesAWrongDecode) {
  // The same graph with Y's cells naming no codec, so the identity decodes
  // them: the Y word `addr | 1` is not a payload start. The heap must say so.
  HeapWorld w;
  Slots rx;
  rx.words.assign(1, 0);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceX, &rx), GRCORE_OK);
  Cell * x1 = make(w.heap, &kCellX, 1);
  rx.words[0] = encX(x1);
  Cell * y1 = make(w.heap, &kCellYForgot, 2);
  x1 = reinterpret_cast<Cell *>(rx.words[0] & kX.mask);
  put(w.heap, x1, &Cell::a, encX(y1));
  put(w.heap, y1, &Cell::a, encY(x1));
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_GT(w.stats().invalid_pointers, 0u);
}

TEST_F(CodecTest, EachRootSourceIsDecodedByItsOwnCodecAndASourceWithNoneByTheHeaps) {
  HeapWorld w;
  Slots rx, ry, rp;
  rx.words.assign(1, 0);
  ry.words.assign(1, 0);
  rp.words.assign(1, 0);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceX, &rx), GRCORE_OK);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceY, &ry), GRCORE_OK);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourcePlain, &rp), GRCORE_OK);
  // Each cell is reachable only through its own source's slot.
  rx.words[0] = encX(make(w.heap, &kCellX, 1));
  ry.words[0] = encY(make(w.heap, &kCellY, 2));
  rp.words[0] = addr(make(w.heap, &kCellX, 3));  // a raw address: the identity
  for (int round = 0; round < 3; round++) {
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  }
  EXPECT_EQ(w.stats().invalid_pointers, 0u);
  EXPECT_EQ(w.stats().live_objects, 3u);
  // Whatever moved, each slot still names its own cell by its own codec.
  EXPECT_TRUE(holds(w, rx.words[0], kX, 1));
  EXPECT_TRUE(holds(w, ry.words[0], kY, 2));
  EXPECT_EQ(reinterpret_cast<Cell *>(rp.words[0])->id, 3u);
}

TEST_F(CodecTest, AControlWithTheYSourceNamingNoCodecLosesItsCell) {
  HeapWorld w;
  Slots ry;
  ry.words.assign(1, 0);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourcePlain, &ry), GRCORE_OK);
  ry.words[0] = encY(make(w.heap, &kCellY, 2));
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_GT(w.stats().invalid_pointers, 0u) << "the identity cannot read a Y word";
  EXPECT_EQ(w.stats().live_objects, 0u) << "and so the cell was not kept";
}

TEST_F(CodecTest, ATypeWhoseCodecIsMalformedIsRefusedAtAllocation) {
  static const GRHEAP_ValueCodec bad_shift = {0, 0, ~UINT64_C(0), 64, 0};
  static const GRHEAP_ValueCodec bad_tag = {1, 2, ~UINT64_C(0), 0, 0};
  static const GRHEAP_Type shift_type = GRHEAP_TYPE_INIT_CODEC(
      "bad", sizeof(Cell), cell_trace, nullptr, nullptr, nullptr, nullptr, &bad_shift);
  static const GRHEAP_Type tag_type = GRHEAP_TYPE_INIT_CODEC(
      "bad", sizeof(Cell), cell_trace, nullptr, nullptr, nullptr, nullptr, &bad_tag);
  HeapWorld w;
  void * p = nullptr;
  EXPECT_FALSE(grheap_type_valid(&shift_type));
  EXPECT_FALSE(grheap_type_valid(&tag_type));
  EXPECT_EQ(grheap_alloc(w.heap, &shift_type, &p), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_alloc(w.heap, &tag_type, &p), GRHEAP_ERR_INVALID);
  EXPECT_TRUE(grheap_type_valid(&kCellX));  // control
  EXPECT_EQ(w.stats().allocations, 0u);
}

TEST_F(CodecTest, ATypeCompiledBeforeTheCodecMemberExistedHasNoCodecAndItsTailIsNotRead) {
  // A type whose struct_size ends at `restore` must not have its `codec` read:
  // the bytes after it are not its own. Plant one there; the heap must decode
  // the type's words with its own (identity) codec and not that one.
  struct Old {
    GRHEAP_Type t;
    const GRHEAP_ValueCodec * planted;
  };
  static Old old = {GRHEAP_TYPE_INIT("old", sizeof(Cell), cell_trace, nullptr, nullptr,
                        nullptr, nullptr),
      &kY};
  old.t.struct_size = offsetof(GRHEAP_Type, codec);
  old.t.codec = &kY;
  EXPECT_TRUE(grheap_type_valid(&old.t));
  HeapWorld w;
  void * root = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  ASSERT_EQ(grheap_alloc(w.heap, &old.t, &root), GRHEAP_OK);
  Cell * c = static_cast<Cell *>(root);
  Cell * other = make(w.heap, &kCellX, 5);
  c = static_cast<Cell *>(root);
  put(w.heap, c, &Cell::a, addr(other));  // an identity word
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().invalid_pointers, 0u);
  EXPECT_EQ(w.stats().live_objects, 2u) << "the identity decoded it, not the planted Y codec";
}

// ---- weak slots -----------------------------------------------------------

namespace {

struct WeakY {
  uint64_t target;
  uint64_t keep;
};
void weak_y_trace(GRHEAP_Tracer * t, void * payload) {
  grheap_trace_word(t, &static_cast<WeakY *>(payload)->keep);
}
const GRHEAP_WeakSpec kWeakYSpec = {GRHEAP_WEAK_CELL, offsetof(WeakY, target), 0, 0, true, nullptr};
const GRHEAP_Type kWeakYType = GRHEAP_TYPE_INIT_CODEC("weak-y", sizeof(WeakY), weak_y_trace,
    nullptr, &kWeakYSpec, nullptr, nullptr, &kY);

}  // namespace

TEST_F(CodecTest, AnEncodedWeakSlotIsDecodedByItsTypesCodecAndClearedWhenTheTargetDies) {
  HeapWorld w;
  Slots rx;
  rx.words.assign(2, 0);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceY, &rx), GRCORE_OK);
  void * wp = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kWeakYType, &wp), GRHEAP_OK);
  rx.words[0] = encY(wp);
  Cell * target = make(w.heap, &kCellX, 1);  // reachable from nothing else
  wp = reinterpret_cast<void *>(rx.words[0] & kY.mask);
  auto * weak = static_cast<WeakY *>(wp);
  ASSERT_EQ(grheap_store_word(w.heap, weak, &weak->target, encY(target)), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  weak = reinterpret_cast<WeakY *>(rx.words[0] & kY.mask);
  EXPECT_EQ(weak->target, 0u) << "the target was unreachable, so the weak slot is cleared";
  EXPECT_EQ(w.stats().weak_cleared, 1u);
  EXPECT_EQ(w.stats().invalid_pointers, 0u);

  // An immediate in the weak slot (a Y immediate that an X decode would take
  // for a reference) is left alone.
  ASSERT_EQ(grheap_store_word(w.heap, weak, &weak->target, immY_looks_like_x(3)), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  weak = reinterpret_cast<WeakY *>(rx.words[0] & kY.mask);
  EXPECT_EQ(weak->target, immY_looks_like_x(3));
  EXPECT_EQ(w.stats().invalid_pointers, 0u);

  // A live target is kept and the slot still names it.
  Cell * live = make(w.heap, &kCellX, 2);
  rx.words[1] = encY(live);
  weak = reinterpret_cast<WeakY *>(rx.words[0] & kY.mask);
  live = reinterpret_cast<Cell *>(rx.words[1] & kY.mask);
  ASSERT_EQ(grheap_store_word(w.heap, weak, &weak->target, encY(live)), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  weak = reinterpret_cast<WeakY *>(rx.words[0] & kY.mask);
  EXPECT_TRUE(holds(w, weak->target, kY, 2));
}

// ---- retention ------------------------------------------------------------

TEST_F(CodecTest, ARetentionPathThroughAnXCellAndAYCellIsFoundWithEachCodec) {
  HeapWorld w;
  Slots rx;
  rx.words.assign(1, 0);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceX, &rx), GRCORE_OK);
  Cell * x1 = make(w.heap, &kCellX, 1);
  rx.words[0] = encX(x1);
  Cell * y1 = make(w.heap, &kCellY, 2);
  x1 = reinterpret_cast<Cell *>(rx.words[0] & kX.mask);
  put(w.heap, x1, &Cell::a, encX(y1));  // y1 is reachable before anything else is allocated
  Cell * leaf = make(w.heap, &kCellX, 3);
  x1 = reinterpret_cast<Cell *>(rx.words[0] & kX.mask);
  y1 = reinterpret_cast<Cell *>(x1->a & kX.mask);
  put(w.heap, y1, &Cell::a, encY(leaf));
  GRHEAP_Retention * path = nullptr;
  ASSERT_EQ(grheap_retention_path(w.heap, leaf, nullptr, &path), GRHEAP_OK);
  EXPECT_TRUE(grheap_retention_retained(path));
  EXPECT_EQ(grheap_retention_step_count(path), 3u) << "x1, y1, leaf";
  grheap_retention_release(path);
  // Control: a cell nothing names is not retained.
  Cell * orphan = make(w.heap, &kCellY, 4);
  ASSERT_EQ(grheap_retention_path(w.heap, orphan, nullptr, &path), GRHEAP_OK);
  EXPECT_FALSE(grheap_retention_retained(path));
  grheap_retention_release(path);
}

// ---- images ---------------------------------------------------------------

namespace {

const GRHEAP_Type * resolve_cells(void * user, const char * name) {
  const GRHEAP_Type ** table = static_cast<const GRHEAP_Type **>(user);
  for (int i = 0; table[i] != nullptr; i++) {
    if (std::strcmp(table[i]->name, name) == 0) {
      return table[i];
    }
  }
  return nullptr;
}

struct TwoEngineWorld {
  HeapWorld w;
  Slots rx, ry;
  explicit TwoEngineWorld(const GRCORE_RootSource * sx, const GRCORE_RootSource * sy) {
    rx.words.assign(1, 0);
    ry.words.assign(1, 0);
    EXPECT_EQ(grcore_context_add_root_source(w.ctx, sx, &rx), GRCORE_OK);
    EXPECT_EQ(grcore_context_add_root_source(w.ctx, sy, &ry), GRCORE_OK);
  }
};

struct Taken {
  GRHEAP_Image * image = nullptr;
  ~Taken() { grheap_image_release(image); }
};

void build_pair(TwoEngineWorld & s, const GRHEAP_Type * tx, const GRHEAP_Type * ty) {
  Cell * x1 = make(s.w.heap, tx, 1);
  s.rx.words[0] = encX(x1);
  Cell * y1 = make(s.w.heap, ty, 2);
  s.ry.words[0] = encY(y1);
  x1 = reinterpret_cast<Cell *>(s.rx.words[0] & kX.mask);
  put(s.w.heap, x1, &Cell::a, encX(y1));          // X cell -> Y cell
  put(s.w.heap, x1, &Cell::b, immX_looks_like_y(5));
  put(s.w.heap, y1, &Cell::a, encY(x1));          // Y cell -> X cell
  put(s.w.heap, y1, &Cell::b, immY_looks_like_x(6));
}

}  // namespace

TEST_F(CodecTest, AnImageOfTwoEnginesRoundTripsAndEveryWordIsRebuiltByItsOwnCodec) {
  TwoEngineWorld src(&kSourceX, &kSourceY);
  build_pair(src, &kCellX, &kCellY);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  EXPECT_EQ(grheap_image_object_count(t.image), 2u);
  EXPECT_EQ(grheap_image_root_count(t.image), 2u);

  TwoEngineWorld dst(&kSourceX, &kSourceY);
  const GRHEAP_Type * table[] = {&kCellX, &kCellY, nullptr};
  GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT(nullptr, nullptr);
  env.resolve = resolve_cells;
  env.user = table;
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  EXPECT_EQ(dst.w.stats().live_objects, 2u);
  ASSERT_TRUE(holds(dst.w, dst.rx.words[0], kX, 1));
  ASSERT_TRUE(holds(dst.w, dst.ry.words[0], kY, 2));
  const Cell * x1 = reinterpret_cast<const Cell *>(dst.rx.words[0] & kX.mask);
  const Cell * y1 = reinterpret_cast<const Cell *>(dst.ry.words[0] & kY.mask);
  EXPECT_EQ(x1->a, encX(y1));
  EXPECT_EQ(y1->a, encY(x1));
  EXPECT_EQ(x1->b, immX_looks_like_y(5));
  EXPECT_EQ(y1->b, immY_looks_like_x(6));
  ASSERT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
  EXPECT_EQ(dst.w.stats().live_objects, 2u);
  EXPECT_EQ(dst.w.stats().invalid_pointers, 0u);
}

TEST_F(CodecTest, AnImageIsRefusedWhenADestinationTypeDisagreesAboutTheEncoding) {
  TwoEngineWorld src(&kSourceX, &kSourceY);
  build_pair(src, &kCellX, &kCellY);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);

  TwoEngineWorld dst(&kSourceX, &kSourceY);
  const GRHEAP_Type * table[] = {&kCellXother, &kCellY, nullptr};
  GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT(nullptr, nullptr);
  env.resolve = resolve_cells;
  env.user = table;
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
  EXPECT_EQ(dst.w.stats().live_objects, 0u) << "nothing was made";
  EXPECT_EQ(dst.rx.words[0], 0u);
  EXPECT_EQ(dst.ry.words[0], 0u);
}

TEST_F(CodecTest, AnImageIsRefusedWhenADestinationRootSourceDisagreesAndNothingIsLeftBehind) {
  TwoEngineWorld src(&kSourceX, &kSourceY);
  build_pair(src, &kCellX, &kCellY);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);

  TwoEngineWorld dst(&kSourceX, &kSourceYother);
  const GRHEAP_Type * table[] = {&kCellX, &kCellY, nullptr};
  GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT(nullptr, nullptr);
  env.resolve = resolve_cells;
  env.user = table;
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
  EXPECT_EQ(dst.w.stats().live_objects, 0u) << "the objects made before PREPARE were removed";
  EXPECT_EQ(dst.rx.words[0], 0u);
  EXPECT_EQ(dst.ry.words[0], 0u);
  // Control: the matching destination takes it.
  TwoEngineWorld ok(&kSourceX, &kSourceY);
  EXPECT_EQ(grheap_image_instantiate(ok.w.heap, t.image, &env), GRHEAP_OK);
}

// ---- a codec per slot ------------------------------------------------------

namespace {

// A third engine's word, shifted so that no other codec's encoding of the same
// address is equal to it: a reference is `(address << 4) | 5`.
const GRHEAP_ValueCodec kZ = {0xF, 0x5, ~UINT64_C(0xF), 4, 0};
uint64_t encZ(const void * p) { return (addr(p) << 4) | 5u; }
uint64_t immZ(uint64_t n) { return (n << 4) | 6u; }

// A Y cell whose slot b holds a Z word directly: a reference or an immediate
// of the other engine, with no box.
void mixed_trace(GRHEAP_Tracer * t, void * payload) {
  auto * c = static_cast<Cell *>(payload);
  grheap_trace_word(t, &c->a);                 // the type's codec (Y)
  grheap_trace_word_with(t, &c->b, &kZ);       // this slot's own (Z)
}
const GRHEAP_Type kMixed = GRHEAP_TYPE_INIT_CODEC(
    "mixed", sizeof(Cell), mixed_trace, nullptr, nullptr, nullptr, nullptr, &kY);
// The control: the same layout with slot b reported by the type's codec.
const GRHEAP_Type kMixedPlain = GRHEAP_TYPE_INIT_CODEC(
    "mixed", sizeof(Cell), cell_trace, nullptr, nullptr, nullptr, nullptr, &kY);

}  // namespace

TEST_F(CodecTest, ASlotWithItsOwnCodecHoldsAnotherEnginesWordsWithNoBox) {
  HeapWorld w;
  Slots ry;
  ry.words.assign(1, 0);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceY, &ry), GRCORE_OK);
  Cell * m = make(w.heap, &kMixed, 1);
  ry.words[0] = encY(m);
  Cell * x1 = make(w.heap, &kCellX, 2);
  m = reinterpret_cast<Cell *>(ry.words[0] & kY.mask);
  put(w.heap, m, &Cell::b, encZ(x1));   // an X reference in a Y cell's slot b
  put(w.heap, m, &Cell::a, immY_looks_like_x(4));
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().invalid_pointers, 0u);
  EXPECT_EQ(w.stats().live_objects, 2u) << "the X cell is kept by the X word in slot b";
  m = reinterpret_cast<Cell *>(ry.words[0] & kY.mask);
  EXPECT_TRUE(holds(w, m->b, kZ, 2));
  // An X immediate in slot b is left alone, even one a Y decode would take.
  put(w.heap, m, &Cell::b, immZ(8));
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().invalid_pointers, 0u);
  EXPECT_EQ(w.stats().live_objects, 1u);
}

TEST_F(CodecTest, AControlReportingTheSameSlotByTheTypesCodecLosesTheCell) {
  HeapWorld w;
  Slots ry;
  ry.words.assign(1, 0);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kSourceY, &ry), GRCORE_OK);
  Cell * m = make(w.heap, &kMixedPlain, 1);
  ry.words[0] = encY(m);
  Cell * x1 = make(w.heap, &kCellX, 2);
  m = reinterpret_cast<Cell *>(ry.words[0] & kY.mask);
  put(w.heap, m, &Cell::b, encZ(x1));
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().live_objects, 1u) << "Y cannot read the X word, so the X cell died";
}

TEST_F(CodecTest, ANullOrMalformedCodecForASlotIsIgnoredNotUsed) {
  static const GRHEAP_ValueCodec bad = {0, 0, ~UINT64_C(0), 64, 0};
  struct Probe {
    static void trace(GRHEAP_Tracer * t, void * p) {
      auto * c = static_cast<Cell *>(p);
      grheap_trace_word_with(t, &c->a, nullptr);
      grheap_trace_word_with(t, &c->b, &bad);
    }
  };
  static const GRHEAP_Type probe = GRHEAP_TYPE_INIT("probe", sizeof(Cell), Probe::trace,
      nullptr, nullptr, nullptr, nullptr);
  HeapWorld w;
  void * root = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  ASSERT_EQ(grheap_alloc(w.heap, &probe, &root), GRHEAP_OK);
  Cell * other = make(w.heap, &kCellX, 2);
  auto * c = static_cast<Cell *>(root);
  put(w.heap, c, &Cell::a, addr(other));
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().live_objects, 1u) << "nothing was reported, so the other cell was not kept";
  grheap_trace_word_with(nullptr, nullptr, nullptr);  // and a NULL tracer is harmless
}

TEST_F(CodecTest, AnImageRecordsASlotsOwnCodecAndRebuildsTheWordWithIt) {
  TwoEngineWorld src(&kSourceX, &kSourceY);
  Cell * m = make(src.w.heap, &kMixed, 1);
  src.ry.words[0] = encY(m);
  Cell * x1 = make(src.w.heap, &kCellX, 2);
  src.rx.words[0] = encX(x1);
  m = reinterpret_cast<Cell *>(src.ry.words[0] & kY.mask);
  put(src.w.heap, m, &Cell::b, encZ(x1));
  put(src.w.heap, m, &Cell::a, immY_looks_like_x(3));
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);

  TwoEngineWorld dst(&kSourceX, &kSourceY);
  const GRHEAP_Type * table[] = {&kMixed, &kCellX, nullptr};
  GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT(nullptr, nullptr);
  env.resolve = resolve_cells;
  env.user = table;
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  ASSERT_TRUE(holds(dst.w, dst.ry.words[0], kY, 1));
  const Cell * dm = reinterpret_cast<const Cell *>(dst.ry.words[0] & kY.mask);
  const Cell * dx = reinterpret_cast<const Cell *>(dst.rx.words[0] & kX.mask);
  EXPECT_EQ(dm->b, encZ(dx)) << "slot b is rebuilt as a Z word, not a Y one";
  EXPECT_EQ(dm->a, immY_looks_like_x(3));
  ASSERT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
  EXPECT_EQ(dst.w.stats().live_objects, 2u);
  EXPECT_EQ(dst.w.stats().invalid_pointers, 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
