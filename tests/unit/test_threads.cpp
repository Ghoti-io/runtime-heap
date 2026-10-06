/**
 * @file
 *
 * A heap migrates with its context: released on one thread, acquired on
 * another, with its objects, handles, pins and roots intact. Run under
 * ThreadSanitizer (`make test-tsan`), where the hand-off through the
 * context's owner word is what makes this free of data races.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <thread>

class ThreadsTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

TEST_F(ThreadsTest, AHeapAndItsHandlesPinsAndRootsMigrateWithTheContext) {
  HeapWorld w(unmoved());
  void * slot = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &slot), GRHEAP_OK);
  Node * rooted = new_node(w.heap, 1);
  slot = rooted;
  Node * handled = new_node(w.heap, 2);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, handled, &h), GRHEAP_OK);
  Node * pinned = new_node(w.heap, 3);
  ASSERT_EQ(grheap_pin(w.heap, pinned), GRHEAP_OK);
  Node * garbage = new_node(w.heap, 4);

  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);

  // While nobody owns the context, nobody may use the heap.
  EXPECT_EQ(grheap_collect(w.heap), GRHEAP_ERR_INVALID);
  void * none;
  EXPECT_EQ(grheap_alloc(w.heap, &kLeaf, &none), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_handle_get(h), nullptr);

  GRHEAP_Result seen_by_other = GRHEAP_OK;
  Node * made_there = nullptr;
  std::thread other([&] {
    EXPECT_EQ(grheap_collect(w.heap), GRHEAP_ERR_INVALID); // not yet owner
    ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    EXPECT_EQ(grheap_handle_get(h), handled);
    seen_by_other = grheap_collect(w.heap);
    made_there = new_node(w.heap, 5);
    ASSERT_EQ(grheap_handle_set(h, made_there), GRHEAP_OK);
    ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  });
  other.join();
  EXPECT_EQ(seen_by_other, GRHEAP_OK);

  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  // The collection on the other thread freed the garbage and kept the rest.
  EXPECT_TRUE(grheap_contains(w.heap, rooted));
  EXPECT_TRUE(grheap_contains(w.heap, handled));
  EXPECT_TRUE(grheap_contains(w.heap, pinned));
  // (Not contains(garbage): the object made on the other thread reuses the
  // freed cell, so the address is live again. The hook is what says it died.)
  EXPECT_TRUE(destroyed(4));
  (void)garbage;
  EXPECT_EQ(grheap_handle_get(h), made_there);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, handled)); // the handle moved on
  EXPECT_TRUE(grheap_contains(w.heap, made_there));
  ASSERT_EQ(grheap_unpin(w.heap, pinned), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &slot), GRHEAP_OK);
  EXPECT_EQ(destroyed_count(4), 1u);
  EXPECT_EQ(destroyed_count(2), 1u);
}

TEST_F(ThreadsTest, ManyHandOffsKeepTheHeapConsistent) {
  HeapWorld w;
  void * head = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &head), GRHEAP_OK);
  constexpr int kRounds = 20;
  for (int round = 0; round < kRounds; round++) {
    ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
    std::thread t([&, round] {
      ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
      for (int i = 0; i < 200; i++) {
        Node * n = new_node(w.heap, 0, &kPlainNode);
        if (i % 10 == 0) {
          ASSERT_EQ(grheap_store(w.heap, n, &n->a, head), GRHEAP_OK);
          head = n; // a tenth of what is made is kept
        }
      }
      ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
      (void)round;
      ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
    });
    t.join();
    ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  }
  EXPECT_EQ(w.stats().live_objects, static_cast<uint64_t>(kRounds * 20));
  head = nullptr;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().live_objects, 0u);
  ASSERT_EQ(grheap_root_remove(w.heap, &head), GRHEAP_OK);
}

TEST_F(ThreadsTest, ARootSourceIsAskedByWhicheverThreadOwnsTheContext) {
  HeapWorld w(unmoved());
  Node * n = new_node(w.heap, 1);
  uint64_t slot = addr(n);
  HeldRoots held;
  held.slots = {&slot};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held),
      GRCORE_OK);
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  std::thread t([&] {
    ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  });
  t.join();
  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  EXPECT_TRUE(grheap_contains(w.heap, n));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
