/**
 * @file
 *
 * `gc_store`: the sanctioned pointer stores, their counters, and that a
 * stored pointer is a real edge.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <thread>

class StoreTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

TEST_F(StoreTest, StoreWritesTheSlotAndCountsIt) {
  HeapWorld w;
  Node * holder = new_node(w.heap, 1);
  Node * child = new_node(w.heap, 2);
  ASSERT_EQ(grheap_store(w.heap, holder, &holder->a, child), GRHEAP_OK);
  EXPECT_EQ(holder->a, child);
  EXPECT_EQ(w.stats().stores, 1u);
  ASSERT_EQ(grheap_store(w.heap, holder, &holder->a, nullptr), GRHEAP_OK);
  EXPECT_EQ(holder->a, nullptr);
  EXPECT_EQ(w.stats().stores, 2u);
}

TEST_F(StoreTest, AStoredPointerKeepsItsTargetAliveThroughTheHolder) {
  HeapWorld w(unmoved());
  Node * holder = new_node(w.heap, 1);
  Node * child = new_node(w.heap, 2);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, holder, &h), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, holder, &holder->b, child), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, child));
}

TEST_F(StoreTest, AHolderMayBeNullForASlotThatIsNotInsideAnyObject) {
  HeapWorld w;
  Node * child = new_node(w.heap, 1);
  void * slot = nullptr;
  ASSERT_EQ(grheap_store(w.heap, nullptr, &slot, child), GRHEAP_OK);
  EXPECT_EQ(slot, child);
  uint64_t word = 0;
  ASSERT_EQ(grheap_store_word(w.heap, nullptr, &word, 0x55AA), GRHEAP_OK);
  EXPECT_EQ(word, 0x55AAu);
  EXPECT_EQ(w.stats().stores, 2u);
}

TEST_F(StoreTest, StoreRangeCopiesEverySlotAndCountsEach) {
  HeapWorld w;
  Node * holder = new_node(w.heap, 1);
  void * values[4] = {holder, nullptr, w.heap, &values};
  void * dst[4] = {};
  ASSERT_EQ(grheap_store_range(w.heap, nullptr, dst, values, 4), GRHEAP_OK);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(dst[i], values[i]) << i;
  }
  EXPECT_EQ(w.stats().stores, 4u);
  // Zero slots is a successful no-op, and takes null pointers.
  ASSERT_EQ(grheap_store_range(w.heap, nullptr, nullptr, nullptr, 0), GRHEAP_OK);
  EXPECT_EQ(w.stats().stores, 4u);
}

TEST_F(StoreTest, StoreRangeCopiesLikeMemmoveWhenTheRangesOverlap) {
  HeapWorld w;
  void * slots[6] = {reinterpret_cast<void *>(1), reinterpret_cast<void *>(2),
      reinterpret_cast<void *>(3), reinterpret_cast<void *>(4),
      reinterpret_cast<void *>(5), reinterpret_cast<void *>(6)};
  ASSERT_EQ(grheap_store_range(w.heap, nullptr, &slots[2], &slots[0], 4),
      GRHEAP_OK);
  const uintptr_t want[6] = {1, 2, 1, 2, 3, 4};
  for (int i = 0; i < 6; i++) {
    EXPECT_EQ(reinterpret_cast<uintptr_t>(slots[i]), want[i]) << i;
  }
}

TEST_F(StoreTest, StoreWordWritesAnEncodedValue) {
  HeapWorld w;
  Node * holder = new_node(w.heap, 1);
  uint64_t * slot = reinterpret_cast<uint64_t *>(&holder->id);
  ASSERT_EQ(grheap_store_word(w.heap, holder, slot, 0xFEEDFACE), GRHEAP_OK);
  EXPECT_EQ(holder->id, 0xFEEDFACEu);
  EXPECT_EQ(w.stats().stores, 1u);
}

TEST_F(StoreTest, StoresRefuseNullsAndANonOwnerAndWriteNothing) {
  HeapWorld w;
  void * slot = nullptr;
  uint64_t word = 0;
  void * src[1] = {w.heap};
  EXPECT_EQ(grheap_store(nullptr, nullptr, &slot, w.heap), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_store(w.heap, nullptr, nullptr, w.heap), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_store_word(nullptr, nullptr, &word, 1), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_store_word(w.heap, nullptr, nullptr, 1), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_store_range(nullptr, nullptr, &slot, src, 1),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_store_range(w.heap, nullptr, nullptr, src, 1),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_store_range(w.heap, nullptr, &slot, nullptr, 1),
      GRHEAP_ERR_INVALID);
  GRHEAP_Result r[3];
  std::thread other([&] {
    r[0] = grheap_store(w.heap, nullptr, &slot, w.heap);
    r[1] = grheap_store_word(w.heap, nullptr, &word, 9);
    r[2] = grheap_store_range(w.heap, nullptr, &slot, src, 1);
  });
  other.join();
  for (GRHEAP_Result x : r) {
    EXPECT_EQ(x, GRHEAP_ERR_INVALID);
  }
  EXPECT_EQ(slot, nullptr);
  EXPECT_EQ(word, 0u);
  EXPECT_EQ(w.stats().stores, 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

TEST_F(StoreTest, StoreRangeRefusesACountWhoseByteLengthOverflows) {
  HeapWorld w;
  void * slot[1] = {};
  void * value[1] = {w.heap};
  EXPECT_EQ(grheap_store_range(w.heap, nullptr, slot, value,
                SIZE_MAX / sizeof(void *) + 1),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(slot[0], nullptr);
  EXPECT_EQ(w.stats().stores, 0u);
}
