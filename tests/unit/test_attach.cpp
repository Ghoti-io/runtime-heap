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
 * The attach check and the address limit (an engine learns, before it uses a
 * heap the host made, whether every object will fit the address bits its value
 * word encodes; the heap enforces the limit when it takes a block).
 */

#include "../test_helpers.h"

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <gtest/gtest.h>

namespace {

// Heap blocks on this system lie below 2^47 (user space), above 2^40 for any
// mapping the provider gives in practice, so a 40-bit limit is always broken
// and a 48-bit one never is.
struct LimitWorld {
  TrackingAllocator allocator;
  FakePages pages;
  GRCORE_Group * group = nullptr;
  GRCORE_Context * ctx = nullptr;
  GRHEAP_Heap * heap = nullptr;
  explicit LimitWorld(unsigned bits, GRHEAP_Mode mode = GRHEAP_MODE_COLLECTED) {
    EXPECT_EQ(grcore_group_create(allocator.get(), &pages.vtable, &group), GRCORE_OK);
    EXPECT_EQ(grcore_context_create(group, nullptr, &ctx), GRCORE_OK);
    GRHEAP_Options * o;
    EXPECT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK);
    grheap_options_set_mode(o, mode);
    grheap_options_set_torture(o, false);
    if (bits != 0) {
      EXPECT_EQ(grheap_options_set_address_bits(o, bits), GRHEAP_OK);
    }
    EXPECT_EQ(grheap_heap_create(ctx, o, &heap), GRHEAP_OK);
    grheap_options_destroy(o);
  }
  LimitWorld(const LimitWorld &) = delete;
  LimitWorld & operator=(const LimitWorld &) = delete;
  ~LimitWorld() {
    EXPECT_EQ(grcore_context_destroy(ctx), GRCORE_OK);
    EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
    EXPECT_EQ(allocator.live, 0) << "blocks leaked";
    EXPECT_EQ(pages.live, 0) << "mappings leaked";
  }
};

}  // namespace

TEST(Attach, TheDefaultLimitIsTheSystemWidthsAndAnEngineThatEncodesItAttaches) {
  LimitWorld w(0);
  EXPECT_EQ(grheap_address_bits(w.heap), GRHEAP_DEFAULT_ADDRESS_BITS);
  GRHEAP_Attach a = GRHEAP_ATTACH_INIT("engine", 48, 0);
  GRHEAP_AttachReason why = GRHEAP_ATTACH_MODE;
  EXPECT_EQ(grheap_attach_check(w.heap, &a, &why), GRHEAP_OK);
  EXPECT_EQ(why, GRHEAP_ATTACH_OK);
  GRHEAP_Attach wide = GRHEAP_ATTACH_INIT("wide", 64, 0);
  EXPECT_EQ(grheap_attach_check(w.heap, &wide, &why), GRHEAP_OK);
}

TEST(Attach, AnEngineWhoseWordEncodesFewerBitsThanTheHeapsLimitIsRefusedByName) {
  LimitWorld w(0);
  GRHEAP_Attach narrow = GRHEAP_ATTACH_INIT("narrow", 40, 0);
  GRHEAP_AttachReason why = GRHEAP_ATTACH_OK;
  EXPECT_EQ(grheap_attach_check(w.heap, &narrow, &why), GRHEAP_ERR_UNSUPPORTED);
  EXPECT_EQ(why, GRHEAP_ATTACH_ADDRESS_RANGE);
  // Nothing changed: the same heap still serves an engine that fits.
  GRHEAP_Attach fits = GRHEAP_ATTACH_INIT("fits", 48, 0);
  EXPECT_EQ(grheap_attach_check(w.heap, &fits, &why), GRHEAP_OK);
  GRHEAP_Stats s = {};
  ASSERT_EQ(grheap_stats(w.heap, &s), GRHEAP_OK);
  EXPECT_EQ(s.allocations, 0u);
}

TEST(Attach, AHostThatLowersTheLimitLetsTheNarrowEngineAttach) {
  LimitWorld w(40);
  EXPECT_EQ(grheap_address_bits(w.heap), 40u);
  GRHEAP_Attach narrow = GRHEAP_ATTACH_INIT("narrow", 40, 0);
  EXPECT_EQ(grheap_attach_check(w.heap, &narrow, nullptr), GRHEAP_OK);
}

TEST(Attach, ASetLimitIsEnforcedWhenABlockIsTakenAndNothingLeaks) {
  LimitWorld w(40);  // the provider's blocks lie above 2^40
  void * p = nullptr;
  EXPECT_EQ(grheap_alloc(w.heap, &kLeaf, &p), GRHEAP_ERR_OOM);
  GRHEAP_Stats s = {};
  ASSERT_EQ(grheap_stats(w.heap, &s), GRHEAP_OK);
  EXPECT_GT(s.address_refusals, 0u) << "the refusal is counted by name";
  EXPECT_EQ(s.mapped_bytes, 0u);
  EXPECT_EQ(w.pages.live, 0) << "the block was given back";
  // Control: the default limit serves the same allocation.
  LimitWorld ok(0);
  EXPECT_EQ(grheap_alloc(ok.heap, &kLeaf, &p), GRHEAP_OK);
  ASSERT_EQ(grheap_stats(ok.heap, &s), GRHEAP_OK);
  EXPECT_EQ(s.address_refusals, 0u);
}

TEST(Attach, ModesAreCheckedWhenTheEngineNamesThem) {
  LimitWorld collected(0);
  LimitWorld arena(0, GRHEAP_MODE_ARENA);
  GRHEAP_Attach only_collected = GRHEAP_ATTACH_INIT("c", 48, GRHEAP_ATTACH_MODE_COLLECTED);
  GRHEAP_AttachReason why = GRHEAP_ATTACH_OK;
  EXPECT_EQ(grheap_attach_check(collected.heap, &only_collected, &why), GRHEAP_OK);
  EXPECT_EQ(grheap_attach_check(arena.heap, &only_collected, &why), GRHEAP_ERR_UNSUPPORTED);
  EXPECT_EQ(why, GRHEAP_ATTACH_MODE);
}

TEST(Attach, BadArgumentsAreInvalidNotRefusals) {
  LimitWorld w(0);
  GRHEAP_Attach ok = GRHEAP_ATTACH_INIT("e", 48, 0);
  GRHEAP_Attach low = GRHEAP_ATTACH_INIT("e", 8, 0);
  GRHEAP_Attach small = GRHEAP_ATTACH_INIT("e", 48, 0);
  small.size = 1;
  EXPECT_EQ(grheap_attach_check(nullptr, &ok, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_attach_check(w.heap, nullptr, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_attach_check(w.heap, &low, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_attach_check(w.heap, &small, nullptr), GRHEAP_ERR_INVALID);
  GRHEAP_Options * o;
  ASSERT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK);
  EXPECT_EQ(grheap_options_set_address_bits(o, 15), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_set_address_bits(o, 65), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_set_address_bits(nullptr, 40), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_get_address_bits(o), GRHEAP_DEFAULT_ADDRESS_BITS);
  grheap_options_destroy(o);
  EXPECT_EQ(grheap_address_bits(nullptr), 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
