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
 * Geometry independent of the system's page size: the heap's blocks, large
 * mappings, mark bits and sweep are correct when the page provider reports a
 * granule of 4, 16, 64 or 256 KiB, and every mapping it asks for is a whole
 * number of granules.
 */

#include "../test_helpers.h"

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <gtest/gtest.h>

namespace {

const size_t kGranules[] = {4096, 16384, 65536, 262144};

void walk_graph(size_t granule) {
  WorldConfig c;
  c.granule = granule;
  c.relocate = 0;
  HeapWorld w(c);
  EXPECT_EQ(w.pages.vtable.page_size, granule);
  void * head = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &head), GRHEAP_OK);
  const int n = 3000;
  for (int i = 0; i < n; i++) {
    Node * node = new_node(w.heap, static_cast<uint64_t>(i), &kPlainNode);
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(grheap_store(w.heap, node, &node->a, head), GRHEAP_OK);
    head = node;
  }
  // Large objects, over a block and over several granules.
  void * big = nullptr;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 300 * 1024, &big), GRHEAP_OK);
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 70 * 1024, &big), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  GRHEAP_Stats s = w.stats();
  EXPECT_EQ(s.live_objects, static_cast<uint64_t>(n)) << "granule " << granule;
  EXPECT_EQ(s.invalid_pointers, 0u);
  EXPECT_EQ(s.mapped_bytes % granule, 0u) << "granule " << granule;
  head = nullptr;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().live_objects, 0u);
  EXPECT_EQ(w.pages.misaligned, 0) << "a mapping that was not whole granules";
}

}  // namespace

TEST(Granule, TheHeapIsCorrectUnderEveryPageGranule) {
  for (size_t g : kGranules) {
    walk_graph(g);
  }
}

TEST(Granule, AControlProviderThatIsNotWholeGranulesIsSeen) {
  // The instrument itself: a provider told a granule counts a map request
  // that is not a multiple of it.
  FakePages pages;
  pages.granule = 65536;
  void * p = pages.vtable.map(pages.vtable.ctx, 4096);
  EXPECT_EQ(pages.misaligned, 1);
  pages.vtable.unmap(pages.vtable.ctx, p, 4096);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
