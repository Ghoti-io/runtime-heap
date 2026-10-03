/**
 * @file
 *
 * Arena mode: bump allocation that never collects, drops a whole heap at once
 * and still runs every `destroy` hook.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>
#include <thread>

namespace {

WorldConfig arena() {
  WorldConfig c;
  c.mode = GRHEAP_MODE_ARENA;
  return c;
}

size_t block_size() {
  return std::max<size_t>(65536, grcore_page_provider_default()->page_size);
}

} // namespace

class ArenaTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

TEST_F(ArenaTest, ObjectsAreBumpAllocatedZeroFilledAndAligned) {
  HeapWorld w(arena());
  std::vector<void *> objs;
  for (int i = 0; i < 1000; i++) {
    size_t size = static_cast<size_t>(i % 90);
    void * p;
    ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, size, &p), GRHEAP_OK);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u);
    const unsigned char * b = static_cast<const unsigned char *>(p);
    for (size_t k = 0; k < size; k++) {
      ASSERT_EQ(b[k], 0);
    }
    memset(p, 0xCC, size); // dirty it; later objects must still read zero
    objs.push_back(p);
  }
  for (void * p : objs) {
    EXPECT_TRUE(grheap_contains(w.heap, p));
  }
  GRHEAP_Stats s = w.stats();
  EXPECT_EQ(s.live_objects, 1000u);
  // ~1000 small cells (average ~80 bytes) fit in two or three 64 KiB chunks.
  EXPECT_LE(s.mapped_bytes, 3 * block_size());
  EXPECT_GE(s.mapped_bytes, block_size());
}

TEST_F(ArenaTest, CollectSucceedsAndDoesNothingNotEvenForUnreachableObjects) {
  HeapWorld w(arena());
  Node * n = new_node(w.heap, 1);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, n));
  EXPECT_EQ(w.stats().collections, 0u);
  EXPECT_TRUE(destroyed_log().empty());
}

TEST_F(ArenaTest, AutoCollectNeverFiresHoweverMuchIsAllocated) {
  HeapWorld w(arena());
  for (int i = 0; i < 20000; i++) {
    new_node(w.heap, 0, &kPlainNode);
  }
  EXPECT_EQ(w.stats().collections, 0u);
  EXPECT_EQ(w.stats().live_objects, 20000u);
}

TEST_F(ArenaTest, ResetRunsEveryHookReleasesEveryChunkAndKeepsTheHeap) {
  HeapWorld w(arena());
  for (uint64_t i = 0; i < 500; i++) {
    new_node(w.heap, i);
  }
  void * large;
  static const GRHEAP_Type big = {"big", 30000, nullptr, [](void *) {
    destroyed_log().push_back(1000000);
  }, nullptr};
  ASSERT_EQ(grheap_alloc(w.heap, &big, &large), GRHEAP_OK);
  EXPECT_GE(w.pages.live, 2);
  ASSERT_EQ(grheap_arena_reset(w.heap), GRHEAP_OK);
  EXPECT_EQ(destroyed_log().size(), 501u);
  for (uint64_t i = 0; i < 500; i++) {
    EXPECT_EQ(destroyed_count(i), 1u) << i;
  }
  EXPECT_EQ(destroyed_count(1000000), 1u);
  EXPECT_EQ(w.pages.live, 0);
  GRHEAP_Stats s = w.stats();
  EXPECT_EQ(s.live_objects, 0u);
  EXPECT_EQ(s.live_bytes, 0u);
  EXPECT_EQ(s.mapped_bytes, 0u);
  EXPECT_EQ(s.frees, 501u);
  EXPECT_FALSE(grheap_contains(w.heap, large));
  // The heap is still there and still allocates, from a fresh chunk.
  EXPECT_EQ(grheap_heap_get(w.ctx), w.heap);
  Node * again = new_node(w.heap, 7);
  EXPECT_TRUE(grheap_contains(w.heap, again));
  // And a second reset runs only the new object's hook.
  destroyed_log().clear();
  ASSERT_EQ(grheap_arena_reset(w.heap), GRHEAP_OK);
  EXPECT_EQ(destroyed_log(), std::vector<uint64_t>{7});
}

TEST_F(ArenaTest, DestroyingTheContextRunsEveryHookAndLeavesNoBlockOrMapping) {
  {
    HeapWorld w(arena());
    for (uint64_t i = 0; i < 300; i++) {
      new_node(w.heap, i);
    }
    void * large;
    ASSERT_EQ(grheap_alloc_sized(w.heap, &kNode, 20000, &large), GRHEAP_OK);
    static_cast<Node *>(large)->id = 9999;
    EXPECT_TRUE(destroyed_log().empty());
  } // the world's destructor checks allocator.live and pages.live are zero
  EXPECT_EQ(destroyed_log().size(), 301u);
  EXPECT_EQ(destroyed_count(299), 1u);
  EXPECT_EQ(destroyed_count(9999), 1u);
}

TEST_F(ArenaTest, HandlesPinsAndRootsAreAcceptedAndKeepNothingAlive) {
  HeapWorld w(arena());
  Node * n = new_node(w.heap, 1);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, n, &h), GRHEAP_OK);
  EXPECT_EQ(grheap_handle_get(h), n);
  ASSERT_EQ(grheap_pin(w.heap, n), GRHEAP_OK);
  ASSERT_EQ(grheap_pin(w.heap, n), GRHEAP_OK);
  void * slot = n;
  ASSERT_EQ(grheap_root_add(w.heap, &slot), GRHEAP_OK);
  EXPECT_EQ(w.stats().pins, 2u);
  EXPECT_EQ(w.stats().handles, 1u);
  EXPECT_EQ(w.stats().roots, 1u);
  // Bad arguments are refused as in the other mode.
  int local;
  EXPECT_EQ(grheap_pin(w.heap, &local), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_handle_create(w.heap, &local, &h), GRHEAP_ERR_INVALID);
  ASSERT_EQ(grheap_unpin(w.heap, n), GRHEAP_OK);
  ASSERT_EQ(grheap_unpin(w.heap, n), GRHEAP_OK);
  EXPECT_EQ(grheap_unpin(w.heap, n), GRHEAP_ERR_INVALID);
  ASSERT_EQ(grheap_root_remove(w.heap, &slot), GRHEAP_OK);
  // A pin placed and then not undone is dropped by the reset, and so is the
  // handle's target; the handle itself stays, empty.
  ASSERT_EQ(grheap_pin(w.heap, n), GRHEAP_OK);
  ASSERT_EQ(grheap_arena_reset(w.heap), GRHEAP_OK);
  EXPECT_EQ(grheap_handle_get(h), nullptr);
  EXPECT_EQ(w.stats().pins, 0u);
  EXPECT_EQ(w.stats().handles, 1u);
  ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
}

TEST_F(ArenaTest, ResetIsRefusedOutsideArenaModeAndForANonOwner) {
  HeapWorld collected;
  EXPECT_EQ(grheap_arena_reset(collected.heap), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_arena_reset(nullptr), GRHEAP_ERR_INVALID);
  HeapWorld w(arena());
  new_node(w.heap, 1);
  GRHEAP_Result r = GRHEAP_OK;
  std::thread other([&] { r = grheap_arena_reset(w.heap); });
  other.join();
  EXPECT_EQ(r, GRHEAP_ERR_INVALID);
  EXPECT_EQ(w.stats().live_objects, 1u);
  EXPECT_TRUE(destroyed_log().empty());
}

TEST_F(ArenaTest, ResetInsideADestroyHookIsRefused) {
  HeapWorld w(arena());
  static GRHEAP_Result inner;
  static GRHEAP_Heap * the_heap;
  the_heap = w.heap;
  inner = GRHEAP_OK;
  static const GRHEAP_Type resetting = {"resetting", 16, nullptr,
      [](void *) { inner = grheap_arena_reset(the_heap); }, nullptr};
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &resetting, &p), GRHEAP_OK);
  ASSERT_EQ(grheap_arena_reset(w.heap), GRHEAP_OK);
  EXPECT_EQ(inner, GRHEAP_ERR_INVALID);
}

TEST_F(ArenaTest, AFailedAllocationLeavesTheArenaAsItWas) {
  HeapWorld w(arena());
  Node * first = new_node(w.heap, 1);
  GRHEAP_Stats before = w.stats();
  // The chunk is nearly empty, so only an allocation that needs a mapping
  // fails: a large one.
  w.pages.fail = true;
  void * p = reinterpret_cast<void *>(0x1);
  EXPECT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 100000, &p), GRHEAP_ERR_OOM);
  EXPECT_EQ(p, reinterpret_cast<void *>(0x1));
  w.pages.fail = false;
  GRHEAP_Stats after = w.stats();
  EXPECT_EQ(after.live_objects, before.live_objects);
  EXPECT_EQ(after.mapped_bytes, before.mapped_bytes);
  EXPECT_TRUE(grheap_contains(w.heap, first));
  EXPECT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 100000, &p), GRHEAP_OK);
}

TEST_F(ArenaTest, ANewChunkIsOpenedWhenTheCurrentOneCannotHoldTheCell) {
  HeapWorld w(arena());
  // 8,000-byte objects: eight fit in a 64 KiB chunk (cell 8,032), so the
  // ninth opens a second chunk. Compare mapped bytes, not addresses.
  for (int i = 0; i < 8; i++) {
    void * p;
    ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 8000, &p), GRHEAP_OK);
  }
  uint64_t one_chunk = w.stats().mapped_bytes;
  EXPECT_EQ(one_chunk, block_size());
  void * p;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 8000, &p), GRHEAP_OK);
  if (block_size() == 65536) {
    EXPECT_EQ(w.stats().mapped_bytes, 2 * block_size());
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
