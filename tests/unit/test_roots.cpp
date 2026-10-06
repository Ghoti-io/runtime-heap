/**
 * @file
 *
 * Registered root slots, handles and pins.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <thread>

class RootsTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

TEST_F(RootsTest, ARootSlotKeepsWhateverItHoldsAtCollectionTime) {
  HeapWorld w(unmoved());
  void * slot = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &slot), GRHEAP_OK);
  EXPECT_EQ(w.stats().roots, 1u);
  Node * first = new_node(w.heap, 1);
  slot = first;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, first));
  // The slot is read each time: pointing it elsewhere frees the first.
  Node * second = new_node(w.heap, 2);
  slot = second;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, first));
  EXPECT_TRUE(grheap_contains(w.heap, second));
  // A removed slot keeps nothing.
  ASSERT_EQ(grheap_root_remove(w.heap, &slot), GRHEAP_OK);
  EXPECT_EQ(w.stats().roots, 0u);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, second));
}

TEST_F(RootsTest, AddAndRemoveRefuseNullDuplicatesAndStrangers) {
  HeapWorld w;
  void * slot = nullptr;
  void * other = nullptr;
  EXPECT_EQ(grheap_root_add(nullptr, &slot), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_root_add(w.heap, nullptr), GRHEAP_ERR_INVALID);
  ASSERT_EQ(grheap_root_add(w.heap, &slot), GRHEAP_OK);
  EXPECT_EQ(grheap_root_add(w.heap, &slot), GRHEAP_ERR_INVALID); // twice
  EXPECT_EQ(w.stats().roots, 1u);
  EXPECT_EQ(grheap_root_remove(w.heap, &other), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_root_remove(w.heap, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_root_remove(nullptr, &slot), GRHEAP_ERR_INVALID);
  ASSERT_EQ(grheap_root_remove(w.heap, &slot), GRHEAP_OK);
  EXPECT_EQ(grheap_root_remove(w.heap, &slot), GRHEAP_ERR_INVALID); // gone
}

TEST_F(RootsTest, ManyRootSlotsGrowTheTableAndRemovalKeepsTheRest) {
  HeapWorld w;
  std::vector<void *> slots(100, nullptr);
  for (void *& s : slots) {
    ASSERT_EQ(grheap_root_add(w.heap, &s), GRHEAP_OK);
  }
  for (size_t i = 0; i < slots.size(); i++) {
    slots[i] = new_node(w.heap, i, &kPlainNode);
  }
  EXPECT_EQ(w.stats().roots, 100u);
  for (size_t i = 0; i < slots.size(); i += 2) {
    ASSERT_EQ(grheap_root_remove(w.heap, &slots[i]), GRHEAP_OK);
  }
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  for (size_t i = 0; i < slots.size(); i++) {
    EXPECT_EQ(grheap_contains(w.heap, slots[i]), i % 2 == 1) << i;
  }
  for (size_t i = 1; i < slots.size(); i += 2) {
    ASSERT_EQ(grheap_root_remove(w.heap, &slots[i]), GRHEAP_OK);
  }
}

TEST_F(RootsTest, ARootSlotHoldingGarbageIsSkippedAndCountedEveryCollection) {
  HeapWorld w;
  int local;
  void * slot = &local;
  ASSERT_EQ(grheap_root_add(w.heap, &slot), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().invalid_pointers, 2u);
  ASSERT_EQ(grheap_root_remove(w.heap, &slot), GRHEAP_OK);
}

TEST_F(RootsTest, AHandleHoldsAnObjectGetAndSetWork) {
  HeapWorld w(steady());
  Node * a = new_node(w.heap, 1);
  Node * b = new_node(w.heap, 2);
  GRHEAP_Handle * h = nullptr;
  ASSERT_EQ(grheap_handle_create(w.heap, a, &h), GRHEAP_OK);
  EXPECT_EQ(w.stats().handles, 1u);
  EXPECT_EQ(grheap_handle_get(h), a);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, a));
  EXPECT_FALSE(grheap_contains(w.heap, b));
  Node * c = new_node(w.heap, 3);
  ASSERT_EQ(grheap_handle_set(h, c), GRHEAP_OK);
  EXPECT_EQ(grheap_handle_get(h), c);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, a));
  EXPECT_TRUE(grheap_contains(w.heap, c));
  // An empty handle is allowed and holds nothing.
  ASSERT_EQ(grheap_handle_set(h, nullptr), GRHEAP_OK);
  EXPECT_EQ(grheap_handle_get(h), nullptr);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, c));
  ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
  EXPECT_EQ(w.stats().handles, 0u);
}

TEST_F(RootsTest, HandlesRefuseWhatIsNotALivePayloadStartAndLeaveThingsAlone) {
  HeapWorld w;
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * h = nullptr;
  int local;
  EXPECT_EQ(grheap_handle_create(w.heap, &local, &h), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_handle_create(w.heap, reinterpret_cast<char *>(a) + 8, &h),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(h, nullptr);
  EXPECT_EQ(grheap_handle_create(nullptr, a, &h), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_handle_create(w.heap, a, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(w.stats().handles, 0u);
  ASSERT_EQ(grheap_handle_create(w.heap, a, &h), GRHEAP_OK);
  EXPECT_EQ(grheap_handle_set(h, &local), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_handle_get(h), a); // unchanged
  EXPECT_EQ(grheap_handle_set(nullptr, a), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_handle_destroy(nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_handle_get(nullptr), nullptr);
}

TEST_F(RootsTest, HandlesAreDestroyedInAnyOrderAndTheRestKeepTheirObjects) {
  HeapWorld w(unmoved());
  std::vector<GRHEAP_Handle *> handles;
  std::vector<Node *> nodes;
  for (uint64_t i = 0; i < 5; i++) {
    nodes.push_back(new_node(w.heap, i, &kPlainNode));
    GRHEAP_Handle * h;
    ASSERT_EQ(grheap_handle_create(w.heap, nodes.back(), &h), GRHEAP_OK);
    handles.push_back(h);
  }
  for (size_t i : {size_t(2), size_t(0), size_t(4)}) { // middle, head, tail
    ASSERT_EQ(grheap_handle_destroy(handles[i]), GRHEAP_OK);
  }
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().handles, 2u);
  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(grheap_contains(w.heap, nodes[i]), i == 1 || i == 3) << i;
  }
}

TEST_F(RootsTest, ALivePinKeepsAnObjectAndEveryPinMustBeUndone) {
  HeapWorld w;
  Node * n = new_node(w.heap, 1);
  ASSERT_EQ(grheap_pin(w.heap, n), GRHEAP_OK);
  ASSERT_EQ(grheap_pin(w.heap, n), GRHEAP_OK);
  ASSERT_EQ(grheap_pin(w.heap, n), GRHEAP_OK);
  EXPECT_EQ(w.stats().pins, 3u);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, n));
  ASSERT_EQ(grheap_unpin(w.heap, n), GRHEAP_OK);
  ASSERT_EQ(grheap_unpin(w.heap, n), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, n)); // one pin left
  ASSERT_EQ(grheap_unpin(w.heap, n), GRHEAP_OK);
  EXPECT_EQ(w.stats().pins, 0u);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, n));
}

TEST_F(RootsTest, ThePinCountStopsAt255AndThe256thIsALimit) {
  HeapWorld w;
  Node * n = new_node(w.heap, 1);
  for (unsigned i = 0; i < GRHEAP_PIN_MAX; i++) {
    ASSERT_EQ(grheap_pin(w.heap, n), GRHEAP_OK) << i;
  }
  EXPECT_EQ(GRHEAP_PIN_MAX, 255u);
  EXPECT_EQ(grheap_pin(w.heap, n), GRHEAP_ERR_LIMIT);
  EXPECT_EQ(w.stats().pins, 255u); // the refusal changed nothing
  // Another object in the same block still pins.
  Node * m = new_node(w.heap, 2);
  EXPECT_EQ(grheap_pin(w.heap, m), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, n));
  for (unsigned i = 0; i < GRHEAP_PIN_MAX; i++) {
    ASSERT_EQ(grheap_unpin(w.heap, n), GRHEAP_OK) << i;
  }
  EXPECT_EQ(grheap_unpin(w.heap, n), GRHEAP_ERR_INVALID); // none left
}

TEST_F(RootsTest, PinsRefuseWhatIsNotALivePayloadStartOrHasNoPin) {
  HeapWorld w(steady());
  Node * n = new_node(w.heap, 1);
  Node * other = new_node(w.heap, 2);
  int local;
  EXPECT_EQ(grheap_pin(w.heap, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_pin(w.heap, &local), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_pin(w.heap, reinterpret_cast<char *>(n) + 8),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_pin(nullptr, n), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_unpin(w.heap, n), GRHEAP_ERR_INVALID); // never pinned
  ASSERT_EQ(grheap_pin(w.heap, n), GRHEAP_OK);
  EXPECT_EQ(grheap_unpin(w.heap, other), GRHEAP_ERR_INVALID); // a neighbour
  EXPECT_EQ(grheap_unpin(w.heap, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_unpin(w.heap, &local), GRHEAP_ERR_INVALID);
  EXPECT_EQ(w.stats().pins, 1u);
}

TEST_F(RootsTest, AnObjectPinnedInALargeMappingSurvives) {
  HeapWorld w;
  void * large;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 50000, &large), GRHEAP_OK);
  ASSERT_EQ(grheap_pin(w.heap, large), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, large));
  ASSERT_EQ(grheap_unpin(w.heap, large), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, large));
}

TEST_F(RootsTest, EveryRootingCallRefusesANonOwner) {
  HeapWorld w;
  Node * n = new_node(w.heap, 1);
  void * slot = nullptr;
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, n, &h), GRHEAP_OK);
  ASSERT_EQ(grheap_pin(w.heap, n), GRHEAP_OK);
  GRHEAP_Result results[7];
  void * got = reinterpret_cast<void *>(0x1);
  std::thread other([&] {
    GRHEAP_Handle * h2;
    results[0] = grheap_root_add(w.heap, &slot);
    results[1] = grheap_root_remove(w.heap, &slot);
    results[2] = grheap_handle_create(w.heap, n, &h2);
    results[3] = grheap_handle_set(h, n);
    results[4] = grheap_handle_destroy(h);
    results[5] = grheap_pin(w.heap, n);
    results[6] = grheap_unpin(w.heap, n);
    got = grheap_handle_get(h);
  });
  other.join();
  for (GRHEAP_Result r : results) {
    EXPECT_EQ(r, GRHEAP_ERR_INVALID);
  }
  EXPECT_EQ(got, nullptr);
  EXPECT_EQ(w.stats().handles, 1u);
  EXPECT_EQ(w.stats().pins, 1u);
  EXPECT_EQ(w.stats().roots, 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
