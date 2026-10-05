/**
 * @file
 *
 * Collection: what survives, what is freed, when it runs, and that a hook runs
 * once.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>
#include <thread>
#include <vector>

namespace {

/* A <-> B, nothing else pointing at either. */
struct Cycle {
  Node * a;
  Node * b;
};

Cycle make_cycle(GRHEAP_Heap * heap, uint64_t id_a = 1, uint64_t id_b = 2) {
  Cycle c;
  c.a = new_node(heap, id_a);
  c.b = new_node(heap, id_b);
  EXPECT_EQ(grheap_store(heap, c.a, &c.a->a, c.b), GRHEAP_OK);
  EXPECT_EQ(grheap_store(heap, c.b, &c.b->a, c.a), GRHEAP_OK);
  return c;
}

} // namespace

class CollectTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

namespace {

/* A destroy hook that asks the heap about every object that was allocated,
 * which type.h forbids it to rely on but which must not read freed memory. */
struct HookProbe {
  GRHEAP_Heap * heap = nullptr;
  std::vector<void *> all;
  size_t asked = 0;
  size_t self_missing = 0;
  size_t wrong_type = 0;
  bool inside = false;
};
HookProbe g_probe;

void probing_destroy(void * payload) {
  HookProbe & p = g_probe;
  bool found_self = false;
  for (void * q : p.all) {
    p.asked++;
    bool in = grheap_contains(p.heap, q);
    const GRHEAP_Type * t = grheap_type_of(p.heap, q);
    size_t size = 0;
    GRHEAP_Result r = grheap_size_of(p.heap, q, &size);
    if (in != (t != nullptr) || in != (r == GRHEAP_OK)) {
      p.wrong_type++;
    }
    if (q == payload) {
      found_self = in;
    }
  }
  if (!found_self) {
    p.self_missing++;
  }
}

const GRHEAP_Type kProbed = GRHEAP_TYPE_INIT("probed", sizeof(Node), node_trace, probing_destroy, nullptr, nullptr, nullptr);
const GRHEAP_Type kBigProbed = GRHEAP_TYPE_INIT("big-probed", 300000, nullptr, probing_destroy, nullptr, nullptr, nullptr);

} // namespace

TEST_F(CollectTest, ADestroyHookMayAskTheHeapAboutEveryObjectDuringTheSweep) {
  // Several spans, large ones among them, all dead at once, so the sweep frees
  // spans while hooks of later ones are still to run. A hook that asks "is
  // this a live object" walks the span array, which must hold only spans that
  // still exist (ASan is the instrument that sees a stale entry).
  WorldConfig config;
  config.auto_collect = false; // no collection but the one asked for
  HeapWorld w(steady(config));
  g_probe = HookProbe();
  g_probe.heap = w.heap;
  for (int i = 0; i < 12; i++) {
    void * p = nullptr;
    ASSERT_EQ(grheap_alloc(w.heap, &kBigProbed, &p), GRHEAP_OK);
    g_probe.all.push_back(p);
  }
  for (int i = 0; i < 400; i++) {
    void * p = nullptr;
    ASSERT_EQ(grheap_alloc(w.heap, &kProbed, &p), GRHEAP_OK);
    g_probe.all.push_back(p);
  }
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(g_probe.asked, g_probe.all.size() * g_probe.all.size());
  EXPECT_EQ(g_probe.self_missing, 0u) << "a dying object is still an object while its own hook runs";
  EXPECT_EQ(g_probe.wrong_type, 0u) << "contains, type_of and size_of disagreed";
  EXPECT_EQ(w.stats().live_objects, 0u);
  for (void * p : g_probe.all) {
    EXPECT_FALSE(grheap_contains(w.heap, p));
  }
  EXPECT_EQ(w.pages.live, 0) << "every emptied span was returned";
}

TEST_F(CollectTest, CyclicGarbageIsFreedAndEachHookRunsOnce) {
  HeapWorld w(steady());
  make_cycle(w.heap);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(destroyed_count(1), 1u);
  EXPECT_EQ(destroyed_count(2), 1u);
  GRHEAP_Stats s = w.stats();
  EXPECT_EQ(s.live_objects, 0u);
  EXPECT_EQ(s.frees, 2u);
  EXPECT_EQ(s.collections, 1u);
  // A second collection finds nothing and runs no hook again.
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(destroyed_log().size(), 2u);
}

TEST_F(CollectTest, ACycleKeptByARegisteredRootSlotSurvivesUntilTheSlotIsRemoved) {
  HeapWorld w(steady());
  void * root = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  Cycle c = make_cycle(w.heap);
  root = c.a;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(destroyed_log().empty());
  EXPECT_TRUE(grheap_contains(w.heap, c.a));
  EXPECT_TRUE(grheap_contains(w.heap, c.b)); // reached through A
  root = nullptr; // the slot is still registered, and now holds nothing
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(destroyed_count(1), 1u);
  EXPECT_EQ(destroyed_count(2), 1u);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST_F(CollectTest, ACycleKeptByAHandleSurvivesUntilTheHandleIsDestroyed) {
  HeapWorld w(steady());
  Cycle c = make_cycle(w.heap);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, c.b, &h), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(destroyed_log().empty());
  EXPECT_EQ(grheap_handle_get(h), c.b);
  ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(destroyed_log().size(), 2u);
}

TEST_F(CollectTest, ACycleKeptByAPinSurvivesUntilItIsUnpinned) {
  HeapWorld w(steady());
  Cycle c = make_cycle(w.heap);
  ASSERT_EQ(grheap_pin(w.heap, c.a), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(destroyed_log().empty());
  ASSERT_EQ(grheap_unpin(w.heap, c.a), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(destroyed_log().size(), 2u);
}

TEST_F(CollectTest, ACycleKeptByARootSourceSlotSurvivesUntilTheSourceIsRemoved) {
  HeapWorld w(steady());
  Cycle c = make_cycle(w.heap);
  uint64_t slot = addr(c.a);
  HeldRoots held;
  held.slots.push_back(&slot);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held),
      GRCORE_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(destroyed_log().empty());
  ASSERT_EQ(grcore_context_remove_root_source(w.ctx, &kHeldSource, &held),
      GRCORE_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(destroyed_log().size(), 2u);
}

TEST_F(CollectTest, OnlyWhatIsUnreachableIsFreed) {
  HeapWorld w(steady());
  Node * keep = new_node(w.heap, 1);
  Node * child = new_node(w.heap, 2);
  Node * orphan = new_node(w.heap, 3);
  ASSERT_EQ(grheap_store(w.heap, keep, &keep->a, child), GRHEAP_OK);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, keep, &h), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, keep));
  EXPECT_TRUE(grheap_contains(w.heap, child));
  EXPECT_FALSE(grheap_contains(w.heap, orphan));
  EXPECT_EQ(destroyed_log(), std::vector<uint64_t>{3});
  // Cutting the only edge to the child frees it at the next collection.
  ASSERT_EQ(grheap_store(w.heap, keep, &keep->a, nullptr), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, child));
  EXPECT_TRUE(grheap_contains(w.heap, keep));
  EXPECT_EQ(destroyed_count(2), 1u);
  EXPECT_EQ(destroyed_count(1), 0u);
}

TEST_F(CollectTest, ALeafObjectsBytesAreNotScannedForPointers) {
  HeapWorld w(steady());
  void * victim = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kPlainNode, &victim), GRHEAP_OK);
  void * leaf = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kLeaf, &leaf), GRHEAP_OK);
  memcpy(leaf, &victim, sizeof victim); // looks like a pointer; is not traced
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, leaf, &h), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, leaf));
  EXPECT_FALSE(grheap_contains(w.heap, victim));
}

TEST_F(CollectTest, ADeepChainIsMarkedWithoutRecursionAndCollectedAfterRelease) {
  HeapWorld w(steady());
  constexpr uint64_t kLength = 100000;
  void * head = nullptr; // a registered root: auto collections run as it grows
  ASSERT_EQ(grheap_root_add(w.heap, &head), GRHEAP_OK);
  for (uint64_t i = 0; i < kLength; i++) {
    Node * n = new_node(w.heap, i);
    // n is unreachable until the next line; nothing allocates
    ASSERT_EQ(grheap_store(w.heap, n, &n->a, head), GRHEAP_OK);
    head = n;
  }
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().live_objects, kLength);
  EXPECT_TRUE(destroyed_log().empty());
  head = nullptr;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().live_objects, 0u);
  EXPECT_EQ(destroyed_log().size(), kLength);
  ASSERT_EQ(grheap_root_remove(w.heap, &head), GRHEAP_OK);
}

TEST_F(CollectTest, AllocationCollectsWhenTheBytesSinceTheLastReachTheThreshold) {
  HeapWorld w;
  // 20,000 unrooted 48-byte cells is 960,000 bytes: at least three trips
  // over the default 256 KiB threshold.
  for (int i = 0; i < 20000; i++) {
    new_node(w.heap, 0, &kPlainNode);
  }
  GRHEAP_Stats s = w.stats();
  EXPECT_GE(s.collections, 3u);
  EXPECT_LT(s.live_objects, 20000u);
  EXPECT_EQ(s.allocations, 20000u);
  EXPECT_EQ(s.frees + s.live_objects, 20000u);
}

TEST_F(CollectTest, TheThresholdIsAnOptionAndASmallOneCollectsSooner) {
  WorldConfig config;
  config.threshold = 4096;
  HeapWorld w(config);
  for (int i = 0; i < 1000; i++) {
    new_node(w.heap, 0, &kPlainNode);
  }
  // 48,000 bytes at 4 KiB: more than ten collections, and few objects left.
  EXPECT_GE(w.stats().collections, 10u);
  EXPECT_LT(w.stats().live_objects, 200u);
}

TEST_F(CollectTest, WithAutoCollectOffAllocationNeverCollectsOnItsOwn) {
  WorldConfig config;
  config.auto_collect = false;
  HeapWorld w(steady(config));
  for (int i = 0; i < 20000; i++) {
    new_node(w.heap, 0, &kPlainNode);
  }
  GRHEAP_Stats s = w.stats();
  EXPECT_EQ(s.collections, 0u);
  EXPECT_EQ(s.live_objects, 20000u);
  // An explicit collection still works.
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().live_objects, 0u);
}

TEST_F(CollectTest, AfterACollectionTheThresholdIsAtLeastTheLiveBytes) {
  HeapWorld w(steady());
  // About 720,000 live bytes, kept by a root.
  void * head = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &head), GRHEAP_OK);
  for (int i = 0; i < 15000; i++) {
    Node * n = new_node(w.heap, static_cast<uint64_t>(i), &kPlainNode);
    ASSERT_EQ(grheap_store(w.heap, n, &n->a, head), GRHEAP_OK);
    head = n;
  }
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  uint64_t base = w.stats().collections;
  ASSERT_EQ(w.stats().live_bytes, 15000u * 48u);
  // 500,000 bytes of garbage is under max(256 KiB, 720,000): no collection.
  for (int i = 0; i < 10400; i++) {
    new_node(w.heap, 0, &kPlainNode);
  }
  EXPECT_EQ(w.stats().collections, base);
  // Another 300,000 passes it, and the next allocation collects.
  for (int i = 0; i < 6300; i++) {
    new_node(w.heap, 0, &kPlainNode);
  }
  EXPECT_EQ(w.stats().collections, base + 1);
  head = nullptr;
  ASSERT_EQ(grheap_root_remove(w.heap, &head), GRHEAP_OK);
}

TEST_F(CollectTest, AThresholdOfZeroCollectsBeforeEveryAllocation) {
  WorldConfig config;
  config.threshold = 0;
  HeapWorld w(config);
  for (int i = 0; i < 25; i++) {
    new_node(w.heap, static_cast<uint64_t>(i));
  }
  // Each allocation collected first, and freed the one before it.
  EXPECT_EQ(w.stats().collections, 25u);
  EXPECT_EQ(w.stats().live_objects, 1u);
  EXPECT_EQ(destroyed_log().size(), 24u);
}

TEST_F(CollectTest, AFreedCellIsReusedAndComesBackZeroFilled) {
  WorldConfig config;
  config.auto_collect = false;
  HeapWorld w(config);
  // One survivor keeps the block alive, so the freed cell stays on its list
  // rather than going back to the provider with the block.
  Node * keep = new_node(w.heap, 1);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, keep, &h), GRHEAP_OK);
  Node * dead = new_node(w.heap, 2);
  ASSERT_EQ(grheap_store(w.heap, dead, &dead->a, dead), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, dead, &dead->b, keep), GRHEAP_OK);
  memset(&dead->id, 0xEE, sizeof dead->id);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, dead));
  Node * again = new_node(w.heap, 3);
  EXPECT_EQ(again, dead); // the one free cell of the class
  EXPECT_EQ(again->a, nullptr);
  EXPECT_EQ(again->b, nullptr);
  EXPECT_EQ(again->id, 3u); // set by new_node, over a zeroed payload
}

TEST_F(CollectTest, ChurningGarbageDoesNotGrowTheHeapWithoutBound) {
  HeapWorld w;
  for (int i = 0; i < 300000; i++) {
    new_node(w.heap, 0, &kPlainNode);
  }
  // 14.4 MB were allocated; the threshold bounds what is held at once.
  EXPECT_LE(w.stats().mapped_bytes, 8u * std::max<size_t>(65536, grcore_page_provider_default()->page_size));
}

TEST_F(CollectTest, EveryObjectInABlockIsFoundWhateverItsSizeClass) {
  // Live objects of many sizes interleaved with garbage of the same sizes: the
  // sweep must free exactly the garbage, across classes and across blocks.
  HeapWorld w;
  std::vector<void *> keep(64, nullptr);
  for (void *& k : keep) {
    ASSERT_EQ(grheap_root_add(w.heap, &k), GRHEAP_OK);
  }
  size_t garbage = 0;
  for (size_t round = 0; round < 40; round++) {
    for (size_t i = 0; i < keep.size(); i++) {
      size_t bytes = 1 + (i * 131 + round * 7) % 6000;
      void * p;
      ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, bytes, &p), GRHEAP_OK);
      if (round == 0) {
        keep[i] = p;
      } else {
        garbage++;
      }
    }
  }
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  GRHEAP_Stats s = w.stats();
  EXPECT_EQ(s.live_objects, keep.size());
  EXPECT_EQ(s.allocations, keep.size() + garbage);
  for (void * k : keep) {
    EXPECT_TRUE(grheap_contains(w.heap, k));
  }
  for (void *& k : keep) {
    ASSERT_EQ(grheap_root_remove(w.heap, &k), GRHEAP_OK);
  }
}

TEST_F(CollectTest, TheHookOfALiveObjectRunsAtContextDestroyAndNotBefore) {
  {
    HeapWorld w;
    Node * n = new_node(w.heap, 7);
    GRHEAP_Handle * h;
    ASSERT_EQ(grheap_handle_create(w.heap, n, &h), GRHEAP_OK);
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    EXPECT_EQ(destroyed_count(7), 0u);
  }
  EXPECT_EQ(destroyed_count(7), 1u);
}

TEST_F(CollectTest, CollectRefusesNullAndANonOwner) {
  HeapWorld w;
  EXPECT_EQ(grheap_collect(nullptr), GRHEAP_ERR_INVALID);
  GRHEAP_Result r = GRHEAP_OK;
  std::thread other([&] { r = grheap_collect(w.heap); });
  other.join();
  EXPECT_EQ(r, GRHEAP_ERR_INVALID);
  EXPECT_EQ(w.stats().collections, 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
