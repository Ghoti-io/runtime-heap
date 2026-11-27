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
 * Weak cells and ephemerons in a heap image: what a weak slot names is in the
 * image only if something else reaches it, the dead are written cleared, an
 * ephemeron's value follows its key to a fixpoint, and a restore registers the
 * weak objects it makes so the next collection judges them.
 */

#include "../test_helpers.h"

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

struct Hold {
  void * a;
  void * b;
  uint64_t id;
};
void hold_trace(GRHEAP_Tracer * t, void * p) {
  auto * h = static_cast<Hold *>(p);
  grheap_trace_slot(t, &h->a);
  grheap_trace_slot(t, &h->b);
}
const GRHEAP_Type kHold = GRHEAP_TYPE_INIT("hold", sizeof(Hold), hold_trace, nullptr,
    nullptr, nullptr, nullptr);

struct WCell {
  void * target;  // weak
  void * strong;
  uint64_t id;
};
void wcell_trace(GRHEAP_Tracer * t, void * p) {
  grheap_trace_slot(t, &static_cast<WCell *>(p)->strong);
}
const GRHEAP_WeakSpec kWCellSpec = {GRHEAP_WEAK_CELL, offsetof(WCell, target), 0, 0, false, nullptr};
const GRHEAP_Type kWCell = GRHEAP_TYPE_INIT("wcell", sizeof(WCell), wcell_trace, nullptr,
    &kWCellSpec, nullptr, nullptr);

struct Eph {
  void * key;
  void * value;
  uint64_t id;
};
const GRHEAP_WeakSpec kEphSpec = {GRHEAP_EPHEMERON, 0, offsetof(Eph, key), offsetof(Eph, value), false, nullptr};
const GRHEAP_Type kEph = GRHEAP_TYPE_INIT("eph", sizeof(Eph), nullptr, nullptr, &kEphSpec,
    nullptr, nullptr);

// A type whose restore refuses, to fail an instantiation after the cells exist.
GRHEAP_Result refuse_restore(GRCORE_Context *, void *, GRHEAP_ImageReader *, void *) {
  return GRHEAP_ERR_INVALID;
}
GRHEAP_Result noop_snapshot(GRCORE_Context *, void *, GRHEAP_ImageWriter * w) {
  return grheap_image_writer_write(w, "x", 1);
}
const GRHEAP_Type kHoldRefuses = GRHEAP_TYPE_INIT("hold", sizeof(Hold), hold_trace, nullptr,
    nullptr, noop_snapshot, refuse_restore);

const GRHEAP_Type * resolve(void * user, const char * name) {
  auto * table = static_cast<const GRHEAP_Type **>(user);
  for (int i = 0; table[i] != nullptr; i++) {
    if (std::strcmp(table[i]->name, name) == 0) {
      return table[i];
    }
  }
  return nullptr;
}

struct Slots {
  std::vector<uint64_t> words;
};
void slots_enumerate(GRCORE_Context *, void * v, const GRCORE_RootVisitor * visitor) {
  for (uint64_t & w : static_cast<Slots *>(v)->words) {
    visitor->slot(visitor->user, &w);
  }
}
const GRCORE_RootSource kSource = GRCORE_ROOT_SOURCE_INIT("slots", slots_enumerate);

struct W {
  HeapWorld w;
  Slots s;
  explicit W(size_t slots, int verify = -1, uint64_t threshold = 0)
      : w(config(verify, threshold)) {
    s.words.assign(slots, 0);
    EXPECT_EQ(grcore_context_add_root_source(w.ctx, &kSource, &s), GRCORE_OK);
  }
  static WorldConfig config(int verify, uint64_t threshold) {
    WorldConfig c = steady();  // no torture: unrooted objects must survive to the take
    if (threshold != 0) {
      c.threshold = threshold;
    }
    c.verify = verify;
    return c;
  }
  template <typename T> T * make(const GRHEAP_Type * type, uint64_t id) {
    void * p = nullptr;
    EXPECT_EQ(grheap_alloc(w.heap, type, &p), GRHEAP_OK);
    static_cast<T *>(p)->id = id;
    return static_cast<T *>(p);
  }
  template <typename T> T * root(size_t i) { return reinterpret_cast<T *>(s.words[i]); }
};

struct Taken {
  GRHEAP_Image * image = nullptr;
  ~Taken() { grheap_image_release(image); }
};

GRHEAP_RestoreEnv env_for(const GRHEAP_Type ** table) {
  GRHEAP_RestoreEnv e = GRHEAP_RESTORE_ENV_INIT(nullptr, nullptr);
  e.resolve = resolve;
  e.user = table;
  return e;
}

const GRHEAP_Type * kTable[] = {&kHold, &kWCell, &kEph, nullptr};

}  // namespace

TEST(ImageWeak, AWeakCellWhoseTargetIsReachedElsewhereKeepsItAndIsJudgedAfterTheRestore) {
  W src(2);
  auto * cell = src.make<WCell>(&kWCell, 1);
  src.s.words[0] = addr(cell);
  auto * target = src.make<Hold>(&kHold, 2);
  src.s.words[1] = addr(target);
  cell = src.root<WCell>(0);
  cell->target = target;
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  EXPECT_EQ(grheap_image_object_count(t.image), 2u);

  W dst(2);
  GRHEAP_RestoreEnv env = env_for(kTable);
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  EXPECT_EQ(dst.w.stats().weak_objects, 1u) << "the restored cell is registered";
  EXPECT_EQ(dst.root<WCell>(0)->target, dst.root<Hold>(1));
  ASSERT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
  EXPECT_EQ(dst.root<WCell>(0)->target, dst.root<Hold>(1)) << "still reached, so still named";
  // Unreach the target: a restored cell is registered, so the collection clears it.
  dst.s.words[1] = 0;
  ASSERT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
  EXPECT_EQ(dst.root<WCell>(0)->target, nullptr);
  EXPECT_EQ(dst.w.stats().weak_cleared, 1u);
}

TEST(ImageWeak, ADeadTargetIsNotInTheImageAndTheRestoredSlotIsCleared) {
  W src(1);
  auto * cell = src.make<WCell>(&kWCell, 1);
  src.s.words[0] = addr(cell);
  auto * dead = src.make<Hold>(&kHold, 2);  // nothing reaches it
  src.root<WCell>(0)->target = dead;
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  EXPECT_EQ(grheap_image_object_count(t.image), 1u) << "the weakly held object is not carried";
  EXPECT_EQ(src.root<WCell>(0)->target, dead) << "the source is untouched";

  W dst(1);
  GRHEAP_RestoreEnv env = env_for(kTable);
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  EXPECT_EQ(dst.root<WCell>(0)->target, nullptr);
  EXPECT_EQ(dst.w.stats().live_objects, 1u);
}

TEST(ImageWeak, AnEphemeronsValueIsInTheImageOnlyWhileItsKeyIs) {
  // Key reached: value comes along.
  {
    W src(2);
    auto * eph = src.make<Eph>(&kEph, 1);
    src.s.words[0] = addr(eph);
    auto * key = src.make<Hold>(&kHold, 2);
    src.s.words[1] = addr(key);
    auto * value = src.make<Hold>(&kHold, 3);  // only through the ephemeron
    eph = src.root<Eph>(0);
    eph->key = key;
    eph->value = value;
    Taken t;
    ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
    EXPECT_EQ(grheap_image_object_count(t.image), 3u);
    W dst(2);
    GRHEAP_RestoreEnv env = env_for(kTable);
    ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
    EXPECT_EQ(dst.root<Eph>(0)->key, dst.root<Hold>(1));
    ASSERT_NE(dst.root<Eph>(0)->value, nullptr);
    EXPECT_EQ(static_cast<Hold *>(dst.root<Eph>(0)->value)->id, 3u);
    // The key dies: the restored ephemeron is cleared by the next collection.
    dst.s.words[1] = 0;
    ASSERT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
    EXPECT_EQ(dst.root<Eph>(0)->key, nullptr);
    EXPECT_EQ(dst.root<Eph>(0)->value, nullptr);
    EXPECT_EQ(dst.w.stats().ephemerons_cleared, 1u);
  }
  // Key not reached: neither comes along, and both slots are cleared.
  {
    W src(1);
    auto * eph = src.make<Eph>(&kEph, 1);
    src.s.words[0] = addr(eph);
    auto * key = src.make<Hold>(&kHold, 2);
    auto * value = src.make<Hold>(&kHold, 3);
    eph = src.root<Eph>(0);
    eph->key = key;
    eph->value = value;
    Taken t;
    ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
    EXPECT_EQ(grheap_image_object_count(t.image), 1u);
    W dst(1);
    GRHEAP_RestoreEnv env = env_for(kTable);
    ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
    EXPECT_EQ(dst.root<Eph>(0)->key, nullptr);
    EXPECT_EQ(dst.root<Eph>(0)->value, nullptr);
  }
}

TEST(ImageWeak, AChainOfEphemeronsIsResolvedToAFixpoint) {
  // eph1 (key K, value V1); V1 strongly holds K2; eph2 (key K2, value V2).
  // V2 is reached only through eph2, whose key is reached only through eph1's
  // value: two rounds of the resolution.
  W src(2);
  auto * eph1 = src.make<Eph>(&kEph, 1);
  src.s.words[0] = addr(eph1);
  auto * eph2 = src.make<Eph>(&kEph, 2);
  src.s.words[1] = addr(eph2);
  auto * k = src.make<Hold>(&kHold, 10);
  auto * k2 = src.make<Hold>(&kHold, 11);
  auto * v1 = src.make<Hold>(&kHold, 20);
  auto * v2 = src.make<Hold>(&kHold, 21);
  // K must be reached strongly for eph1 to hold: hang it from eph2's holder.
  Hold * anchor = src.make<Hold>(&kHold, 30);
  eph1 = src.root<Eph>(0);
  eph2 = src.root<Eph>(1);
  eph1->key = k;
  eph1->value = v1;
  v1->a = k2;
  eph2->key = k2;
  eph2->value = v2;
  anchor->a = k;
  // Root the anchor through eph-free path: replace root 1's chain? Keep eph2
  // rooted and add the anchor to the first root source slot list.
  src.s.words.push_back(addr(anchor));
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  // eph1, eph2, anchor, k, v1, k2, v2.
  EXPECT_EQ(grheap_image_object_count(t.image), 7u);

  W dst(3);
  GRHEAP_RestoreEnv env = env_for(kTable);
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  ASSERT_NE(dst.root<Eph>(1)->value, nullptr);
  EXPECT_EQ(static_cast<Hold *>(dst.root<Eph>(1)->value)->id, 21u);
  ASSERT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
  EXPECT_EQ(dst.w.stats().live_objects, 7u);
}

TEST(ImageWeak, AFailedRestoreLeavesNoWeakEntryForADeadCell) {
  W src(2);
  auto * cell = src.make<WCell>(&kWCell, 1);
  src.s.words[0] = addr(cell);
  auto * h = src.make<Hold>(&kHold, 2);
  src.s.words[1] = addr(h);
  src.root<WCell>(0)->target = h;
  const GRHEAP_Type * refusing[] = {&kHoldRefuses, &kWCell, nullptr};
  Taken t;
  // Take under the refusing type so the image carries its hooks' bytes.
  {
    W s2(2);
    void * p = nullptr;
    ASSERT_EQ(grheap_alloc(s2.w.heap, &kWCell, &p), GRHEAP_OK);
    s2.s.words[0] = addr(p);
    void * q = nullptr;
    ASSERT_EQ(grheap_alloc(s2.w.heap, &kHoldRefuses, &q), GRHEAP_OK);
    s2.s.words[1] = addr(q);
    static_cast<WCell *>(s2.root<WCell>(0))->target = q;
    ASSERT_EQ(grheap_image_take(s2.w.heap, nullptr, &t.image), GRHEAP_OK);
  }
  W dst(2);
  GRHEAP_RestoreEnv env = env_for(refusing);
  EXPECT_NE(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  EXPECT_EQ(dst.w.stats().live_objects, 0u);
  EXPECT_EQ(dst.w.stats().allocations, 2u) << "the failure came after the cells were made";
  EXPECT_EQ(dst.w.stats().weak_objects, 0u)
      << "the registry still names a cell the failed restore removed";
  // The registry holds nothing for the removed cell: a collection and a new
  // weak object behave (the sanitizers would see a stale entry).
  ASSERT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
  void * p = nullptr;
  ASSERT_EQ(grheap_alloc(dst.w.heap, &kWCell, &p), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
}

TEST(ImageWeak, ARestoredWeakObjectPassesBarrierVerify) {
  W src(2);
  auto * cell = src.make<WCell>(&kWCell, 1);
  src.s.words[0] = addr(cell);
  auto * target = src.make<Hold>(&kHold, 2);
  src.s.words[1] = addr(target);
  ASSERT_EQ(grheap_store(src.w.heap, src.root<WCell>(0), &src.root<WCell>(0)->target, target),
      GRHEAP_OK);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  W dst(2, 1 /* count violations */);
  GRHEAP_RestoreEnv env = env_for(kTable);
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
  EXPECT_EQ(dst.w.stats().verify_violations, 0u);
}

/* ---- A failed restore gives back what its tables grew by ------------------ */

namespace {

// A "hold" whose restore refuses at the call numbered `g_refuse_at`.
int g_restore_calls = 0;
int g_refuse_at = -1;
GRHEAP_Result counted_restore(GRCORE_Context *, void *, GRHEAP_ImageReader *, void *) {
  return g_restore_calls++ == g_refuse_at ? GRHEAP_ERR_INVALID : GRHEAP_OK;
}
const GRHEAP_Type kHoldCounted = GRHEAP_TYPE_INIT("hold", sizeof(Hold), hold_trace, nullptr,
    nullptr, noop_snapshot, counted_restore);
const GRHEAP_Type * kCountedTable[] = {&kHoldCounted, &kWCell, nullptr};

constexpr size_t kMany = 20;  // past the sixteen every table starts with

struct Whole {
  uint64_t objects, bytes, mapped, ids, weak, in_use;
  long blocks;
  static Whole of(W & x) {
    GRHEAP_Stats s = x.w.stats();
    return {s.live_objects, s.live_bytes, s.mapped_bytes, s.ids_live, s.weak_objects,
        grcore_context_memory_in_use(x.w.ctx), x.w.allocator.live};
  }
};
void expect_same(const Whole & a, const Whole & b, const char * what) {
  SCOPED_TRACE(what);
  EXPECT_EQ(a.objects, b.objects);
  EXPECT_EQ(a.bytes, b.bytes);
  EXPECT_EQ(a.mapped, b.mapped);
  EXPECT_EQ(a.ids, b.ids);
  EXPECT_EQ(a.weak, b.weak);
  EXPECT_EQ(a.blocks, b.blocks);
  EXPECT_EQ(a.in_use, b.in_use);
}

// An image that needs more spans, more weak entries and more IDs than the
// first table of each holds: large objects with a mapping apiece, weak cells.
void take_many(Taken & t) {
  W src(2 * kMany);
  for (size_t i = 0; i < kMany; i++) {
    void * big = nullptr;
    EXPECT_EQ(grheap_alloc_sized(src.w.heap, &kHoldCounted, 9000, &big), GRHEAP_OK);
    src.s.words[i] = addr(big);
    auto * cell = src.make<WCell>(&kWCell, i);
    src.s.words[kMany + i] = addr(cell);
    uint64_t id = 0;
    EXPECT_EQ(grheap_id_of(src.w.heap, cell, &id), GRHEAP_OK);
  }
  EXPECT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
}

// Fills a span table to sixteen and empties it, so the destination starts with
// tables it already owns.
void use_and_empty(W & x) {
  for (int i = 0; i < 16; i++) {
    void * p = nullptr;
    EXPECT_EQ(grheap_alloc_sized(x.w.heap, &kHold, 9000, &p), GRHEAP_OK);
  }
  EXPECT_EQ(grheap_collect(x.w.heap), GRHEAP_OK);
  EXPECT_EQ(x.w.stats().live_objects, 0u);
}

void succeeds_afterwards(W & dst, const Taken & t) {
  GRHEAP_RestoreEnv env = env_for(kCountedTable);
  g_refuse_at = -1;
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  EXPECT_EQ(dst.w.stats().live_objects, 2 * kMany);
  EXPECT_EQ(dst.w.stats().ids_live, kMany);
  EXPECT_EQ(dst.w.stats().weak_objects, kMany);
  EXPECT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
}

}  // namespace

TEST(ImageWeak, ARestoreRefusedPartWayLeavesNoGrownTableBehind) {
  Taken t;
  take_many(t);
  for (bool used : {false, true}) {
    for (int at : {0, 10, 19}) {
      W dst(2 * kMany);
      if (used) {
        use_and_empty(dst);
      }
      Whole before = Whole::of(dst);
      g_restore_calls = 0;
      g_refuse_at = at;
      GRHEAP_RestoreEnv env = env_for(kCountedTable);
      EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
      expect_same(before, Whole::of(dst), used ? "a hook, tables in use before" : "a hook");
      succeeds_afterwards(dst, t);
    }
  }
}

// Refused after every object is made and registered: the destination's roots
// are not the ones the image recorded.
TEST(ImageWeak, ARestoreAbandonedForItsRootsLeavesNoGrownTableBehind) {
  Taken t;
  take_many(t);
  W dst(2 * kMany - 1);
  Whole before = Whole::of(dst);
  g_refuse_at = -1;
  GRHEAP_RestoreEnv env = env_for(kCountedTable);
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
  expect_same(before, Whole::of(dst), "abandoned");
}

// The allocator refuses one call, each in turn.
TEST(ImageWeak, EveryAllocationRefusedDuringARestoreLeavesTheHeapAsItWas) {
  Taken t;
  take_many(t);
  long total = 0;
  g_refuse_at = -1;
  {
    W probe(2 * kMany, 0);
    long start = probe.w.allocator.calls;
    GRHEAP_RestoreEnv env = env_for(kCountedTable);
    ASSERT_EQ(grheap_image_instantiate(probe.w.heap, t.image, &env), GRHEAP_OK);
    total = probe.w.allocator.calls - start;
  }
  ASSERT_GT(total, 30);
  for (long k = 1; k <= total; k++) {
    W dst(2 * kMany, 0);
    Whole before = Whole::of(dst);
    dst.w.allocator.fail_at = dst.w.allocator.calls + k;
    GRHEAP_RestoreEnv env = env_for(kCountedTable);
    GRHEAP_Result r = grheap_image_instantiate(dst.w.heap, t.image, &env);
    dst.w.allocator.fail_at = 0;
    ASSERT_EQ(r, GRHEAP_ERR_OOM) << "allocation " << k;
    expect_same(before, Whole::of(dst), ("allocation " + std::to_string(k)).c_str());
    succeeds_afterwards(dst, t);
  }
}

// The bytes a failed restore made do not count toward the next collection.
TEST(ImageWeak, ARefusedRestoreDoesNotBringTheNextCollectionForward) {
  Taken t;
  take_many(t);  // far more than the threshold below
  W dst(2 * kMany, -1, 100000);
  g_restore_calls = 0;
  g_refuse_at = 19;
  GRHEAP_RestoreEnv env = env_for(kCountedTable);
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
  uint64_t collections = dst.w.stats().collections;
  (void)dst.make<WCell>(&kWCell, 1);
  EXPECT_EQ(dst.w.stats().collections, collections);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
