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
 * The GC-point entry: an engine declares each GC point the heap does not own,
 * of a stated kind, and every torture mode acts there; the heap counts the
 * points seen and acted on by kind.
 */

#include "../test_helpers.h"

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <gtest/gtest.h>

namespace {

const GRHEAP_GcPointKind kDeclared[] = {GRHEAP_GC_POINT_ENGINE_POLL,
    GRHEAP_GC_POINT_CALL_OUT, GRHEAP_GC_POINT_FRAME_PUSH,
    GRHEAP_GC_POINT_NESTED_ACTIVATION};

void counts(HeapWorld & w, GRHEAP_GcPointKind k, uint64_t * seen, uint64_t * acted) {
  ASSERT_EQ(grheap_gc_point_counts(w.heap, k, seen, acted), GRHEAP_OK);
}

}  // namespace

TEST(GcPoint, UnderTortureEveryDeclaredKindCollectsAndIsCountedActed) {
  for (GRHEAP_GcPointKind k : kDeclared) {
    WorldConfig c;
    c.torture = 1;
    c.relocate = 0;
    HeapWorld w(c);
    void * p = nullptr;
    ASSERT_EQ(grheap_alloc(w.heap, &kLeaf, &p), GRHEAP_OK);  // held only here
    ASSERT_EQ(w.stats().live_objects, 1u);
    ASSERT_EQ(grheap_gc_point(w.heap, k), GRHEAP_OK);
    EXPECT_EQ(w.stats().live_objects, 0u) << "kind " << k << ": the point collected";
    uint64_t seen = 0, acted = 0;
    counts(w, k, &seen, &acted);
    EXPECT_EQ(seen, 1u);
    EXPECT_EQ(acted, 1u) << "kind " << k;
  }
}

TEST(GcPoint, WithoutATortureModeItIsCountedSeenAndDoesNothingElse) {
  for (GRHEAP_GcPointKind k : kDeclared) {
    WorldConfig c;
    c.torture = 0;
    HeapWorld w(c);
    void * p = nullptr;
    ASSERT_EQ(grheap_alloc(w.heap, &kLeaf, &p), GRHEAP_OK);
    ASSERT_EQ(grheap_gc_point(w.heap, k), GRHEAP_OK);
    EXPECT_EQ(w.stats().live_objects, 1u) << "no collection";
    uint64_t seen = 0, acted = 1;
    counts(w, k, &seen, &acted);
    EXPECT_EQ(seen, 1u);
    EXPECT_EQ(acted, 0u);
  }
}

TEST(GcPoint, TheHeapsOwnPointsAreCountedUnderTheirOwnKinds) {
  WorldConfig c;
  c.torture = 1;
  c.relocate = 0;
  HeapWorld w(c);
  void * p = nullptr;
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(grheap_alloc(w.heap, &kLeaf, &p), GRHEAP_OK);
  }
  uint64_t seen = 0, acted = 0;
  counts(w, GRHEAP_GC_POINT_ALLOCATION, &seen, &acted);
  EXPECT_EQ(seen, 3u);
  EXPECT_EQ(acted, 3u);
  // A kind nothing declared reports zero: the gate an engine writes over it.
  counts(w, GRHEAP_GC_POINT_CALL_OUT, &seen, &acted);
  EXPECT_EQ(seen, 0u);
  EXPECT_EQ(acted, 0u);
}

TEST(GcPoint, ABadKindTheHeapsOwnKindsABusyHeapAndNullAreRefusedAndCountNothing) {
  HeapWorld w;
  EXPECT_EQ(grheap_gc_point(nullptr, GRHEAP_GC_POINT_CALL_OUT), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_gc_point(w.heap, GRHEAP_GC_POINT_ALLOCATION), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_gc_point(w.heap, GRHEAP_GC_POINT_POLL), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_gc_point(w.heap, GRHEAP_GC_POINT_KIND_COUNT), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_gc_point(w.heap, static_cast<GRHEAP_GcPointKind>(-1)), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_gc_point_counts(w.heap, GRHEAP_GC_POINT_KIND_COUNT, nullptr, nullptr),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_gc_point_counts(nullptr, GRHEAP_GC_POINT_CALL_OUT, nullptr, nullptr),
      GRHEAP_ERR_INVALID);
  uint64_t seen = 7;
  counts(w, GRHEAP_GC_POINT_CALL_OUT, &seen, nullptr);
  EXPECT_EQ(seen, 0u);
}

namespace {
GRHEAP_Result g_inside = GRHEAP_OK;
GRHEAP_Heap * g_heap = nullptr;
void point_in_trace(GRHEAP_Tracer *, void *) {
  g_inside = grheap_gc_point(g_heap, GRHEAP_GC_POINT_CALL_OUT);
}
const GRHEAP_Type kPointer = GRHEAP_TYPE_INIT("pointer", 16, point_in_trace, nullptr,
    nullptr, nullptr, nullptr);
}  // namespace

TEST(GcPoint, AnEngineCannotDeclareAPointFromInsideTrace) {
  HeapWorld w(steady());
  g_heap = w.heap;
  void * root = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  ASSERT_EQ(grheap_alloc(w.heap, &kPointer, &root), GRHEAP_OK);
  g_inside = GRHEAP_OK;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(g_inside, GRHEAP_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
