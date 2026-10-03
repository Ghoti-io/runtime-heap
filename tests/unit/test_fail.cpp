/**
 * @file
 *
 * Allocation failure and the memory budget: every arm that can fail, failing,
 * and the line between a refusal by the budget (LIMIT) and a failure of the
 * allocator or the page provider (OOM).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <functional>

namespace {

/* Fails the Nth allocator call of `op` for N = 1, 2, ... until one succeeds,
 * and holds the library to what a failure must leave: OOM (not LIMIT, since
 * nothing here is over a budget), and nothing the caller can see changed. The
 * world's destructor then checks that no block leaked.
 *
 * `sticky` fails every call from the Nth on, which is what keeps a failure
 * alive through the retry an allocation makes after a collection. */
void sweep(const char * what, bool sticky,
    const std::function<void(HeapWorld &)> & setup,
    const std::function<GRHEAP_Result(HeapWorld &)> & op,
    const WorldConfig & config = WorldConfig()) {
  bool saw_failure = false;
  bool saw_success = false;
  for (long n = 1; n < 60 && !saw_success; n++) {
    HeapWorld w(config);
    setup(w);
    GRHEAP_Stats before = w.stats();
    w.allocator.calls = 0;
    w.allocator.fail_at = n;
    w.allocator.sticky = sticky;
    GRHEAP_Result r = op(w);
    w.allocator.fail_at = 0;
    w.allocator.sticky = false;
    if (r == GRHEAP_OK) {
      saw_success = true;
      break;
    }
    saw_failure = true;
    ASSERT_EQ(r, GRHEAP_ERR_OOM) << what << " N=" << n;
    GRHEAP_Stats after = w.stats();
    EXPECT_EQ(after.live_objects, before.live_objects) << what << " N=" << n;
    EXPECT_EQ(after.allocations, before.allocations) << what << " N=" << n;
    EXPECT_EQ(after.mapped_bytes, before.mapped_bytes) << what << " N=" << n;
    EXPECT_EQ(after.handles, before.handles) << what << " N=" << n;
    EXPECT_EQ(after.pins, before.pins) << what << " N=" << n;
    EXPECT_EQ(after.roots, before.roots) << what << " N=" << n;
  }
  EXPECT_TRUE(saw_failure) << what << ": no allocation ever failed";
  EXPECT_TRUE(saw_success) << what << ": it never succeeded";
}

} // namespace

class FailTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

/* ---- The allocator fails ---------------------------------------------- */

/* An allocation that cannot get memory collects and tries once more, so a
 * failure that happens once is survived: the sweeps below are sticky, failing
 * every call from the Nth on, which is what outlasts the retry. */
TEST_F(FailTest, AllocatingTheFirstObjectOfAClassNeedsABlockAndFailsCleanly) {
  void * out = nullptr;
  sweep("alloc, new block", true, [](HeapWorld &) {},
      [&](HeapWorld & w) { return grheap_alloc(w.heap, &kNode, &out); });
  EXPECT_NE(out, nullptr);
}

TEST_F(FailTest, ASingleAllocatorFailureIsSurvivedByTheRetryAfterACollection) {
  HeapWorld w;
  w.allocator.calls = 0;
  w.allocator.fail_at = 1; // once: the first attempt only
  void * p = nullptr;
  EXPECT_EQ(grheap_alloc(w.heap, &kNode, &p), GRHEAP_OK);
  w.allocator.fail_at = 0;
  EXPECT_NE(p, nullptr);
  EXPECT_EQ(w.stats().collections, 1u); // the retry's collection
}

TEST_F(FailTest, AllocatingWhenTheSpanTableIsFullNeedsItToGrowAndFailsCleanly) {
  // Sixteen oversized objects fill the sixteen-entry span table; the next
  // block or mapping has to grow it.
  std::vector<void *> keep(16, nullptr);
  sweep("alloc, span table growth", true,
      [&](HeapWorld & w) {
        keep.assign(16, nullptr);
        for (size_t i = 0; i < 16; i++) {
          ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 9000, &keep[i]),
              GRHEAP_OK);
          ASSERT_EQ(grheap_root_add(w.heap, &keep[i]), GRHEAP_OK);
        }
      },
      [&](HeapWorld & w) {
        void * p;
        return grheap_alloc_sized(w.heap, &kLeaf, 9000, &p);
      });
}

TEST_F(FailTest, AllocatingALargeObjectFailsCleanly) {
  void * out = nullptr;
  sweep("alloc, large", true, [](HeapWorld &) {},
      [&](HeapWorld & w) {
        return grheap_alloc_sized(w.heap, &kLeaf, 200000, &out);
      });
  EXPECT_NE(out, nullptr);
}

TEST_F(FailTest, AnAllocationThatFitsAnOpenBlockCallsTheAllocatorNotAtAll) {
  HeapWorld w;
  new_node(w.heap, 1);
  w.allocator.calls = 0;
  for (int i = 0; i < 100; i++) {
    new_node(w.heap, 2);
  }
  EXPECT_EQ(w.allocator.calls, 0);
}

TEST_F(FailTest, CreatingAHandleFailsCleanly) {
  Node * n = nullptr;
  GRHEAP_Handle * h = nullptr;
  sweep("handle create", false, [&](HeapWorld & w) { n = new_node(w.heap, 1); },
      [&](HeapWorld & w) { return grheap_handle_create(w.heap, n, &h); });
  EXPECT_NE(h, nullptr);
}

TEST_F(FailTest, AddingARootSlotFailsCleanlyForTheFirstAndForTheGrowth) {
  std::vector<void *> slots(40, nullptr);
  // The first add makes the table; the seventeenth grows it.
  for (size_t already : {size_t(0), size_t(16)}) {
    sweep("root add", false,
        [&](HeapWorld & w) {
          for (size_t i = 0; i < already; i++) {
            ASSERT_EQ(grheap_root_add(w.heap, &slots[i]), GRHEAP_OK);
          }
        },
        [&](HeapWorld & w) { return grheap_root_add(w.heap, &slots[39]); });
  }
}

TEST_F(FailTest, ThePinTableForABlockFailsCleanlyAndIsMadeOnlyOnce) {
  Node * a = nullptr;
  Node * b = nullptr;
  sweep("pin", false,
      [&](HeapWorld & w) {
        a = new_node(w.heap, 1);
        b = new_node(w.heap, 2);
      },
      [&](HeapWorld & w) { return grheap_pin(w.heap, a); });
  // With the table made, a second pin in the same block needs no allocation.
  HeapWorld w;
  a = new_node(w.heap, 1);
  b = new_node(w.heap, 2);
  ASSERT_EQ(grheap_pin(w.heap, a), GRHEAP_OK);
  w.allocator.calls = 0;
  ASSERT_EQ(grheap_pin(w.heap, b), GRHEAP_OK);
  EXPECT_EQ(w.allocator.calls, 0);
}

TEST_F(FailTest, AFailedPinLeavesNothingPinned) {
  HeapWorld w;
  Node * a = new_node(w.heap, 1);
  w.allocator.fail_all = true;
  EXPECT_EQ(grheap_pin(w.heap, a), GRHEAP_ERR_OOM);
  w.allocator.fail_all = false;
  EXPECT_EQ(w.stats().pins, 0u);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, a));
}

/* ---- The page provider fails ------------------------------------------- */

TEST_F(FailTest, APageProviderThatCannotMapIsOomAndChangesNothing) {
  HeapWorld w;
  Node * kept = new_node(w.heap, 1);
  // Rooted: a failed allocation collects before it gives up, and an unrooted
  // object would be what it collected.
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, kept, &h), GRHEAP_OK);
  GRHEAP_Stats before = w.stats();
  w.pages.fail = true;
  void * p = reinterpret_cast<void *>(0x1);
  // A different class needs a new block; a large object needs a mapping.
  EXPECT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 500, &p), GRHEAP_ERR_OOM);
  EXPECT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 90000, &p), GRHEAP_ERR_OOM);
  EXPECT_EQ(p, reinterpret_cast<void *>(0x1));
  EXPECT_EQ(grcore_context_memory_refusals(w.ctx), 0u); // not the budget
  // An allocation that fits the open block does not need the provider.
  EXPECT_EQ(grheap_alloc(w.heap, &kNode, &p), GRHEAP_OK);
  w.pages.fail = false;
  GRHEAP_Stats after = w.stats();
  EXPECT_EQ(after.mapped_bytes, before.mapped_bytes);
  EXPECT_TRUE(grheap_contains(w.heap, kept));
  EXPECT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 500, &p), GRHEAP_OK);
}

TEST_F(FailTest, ABlockThatCannotBeMappedOnceIsMappedOnTheRetryAfterACollection) {
  HeapWorld w;
  // Garbage fills the open block; the next class has no block yet.
  for (int i = 0; i < 100; i++) {
    new_node(w.heap, 0, &kPlainNode);
  }
  uint64_t collections = w.stats().collections;
  w.pages.fail_next = 1; // the first attempt fails, the retry may succeed
  void * p;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, 500, &p), GRHEAP_OK);
  EXPECT_EQ(w.stats().collections, collections + 1);
  EXPECT_EQ(w.pages.fail_next, 0);
}

/* ---- The memory budget ------------------------------------------------- */

TEST_F(FailTest, FillingTheBudgetWithGarbageIsReclaimedNotRefused) {
  WorldConfig config;
  config.memory = 300000;
  config.reserve = 0;
  HeapWorld w(config);
  // 500,000 unrooted cells is 24 MB, far past the budget. Automatic
  // collection keeps the heap inside it, and none of it is refused for good.
  for (int i = 0; i < 500000; i++) {
    void * p;
    ASSERT_EQ(grheap_alloc(w.heap, &kPlainNode, &p), GRHEAP_OK) << i;
  }
  EXPECT_LE(grcore_context_memory_peak(w.ctx), 300000u);
  EXPECT_GE(w.stats().collections, 1u);
}

TEST_F(FailTest, ACollectionThatFreesWhatARefusedBlockNeededMakesTheAllocationSucceed) {
  WorldConfig config;
  config.memory = 200000;
  config.reserve = 0;
  config.auto_collect = false; // only the refusal itself collects
  HeapWorld w(config);
  for (int i = 0; i < 100000; i++) {
    void * p;
    ASSERT_EQ(grheap_alloc(w.heap, &kPlainNode, &p), GRHEAP_OK) << i;
  }
  EXPECT_GE(grcore_context_memory_refusals(w.ctx), 1u);
  EXPECT_GE(w.stats().collections, 1u);
}

TEST_F(FailTest, ALiveHeapPastTheBudgetIsLimitAndNotOutOfMemory) {
  WorldConfig config;
  config.memory = 300000;
  config.reserve = 0;
  HeapWorld w(config);
  void * head = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &head), GRHEAP_OK);
  GRHEAP_Result r = GRHEAP_OK;
  uint64_t made = 0;
  for (; made < 100000; made++) {
    void * p;
    r = grheap_alloc(w.heap, &kPlainNode, &p);
    if (r != GRHEAP_OK) {
      break;
    }
    static_cast<Node *>(p)->a = head;
    head = p;
  }
  EXPECT_EQ(r, GRHEAP_ERR_LIMIT);
  EXPECT_GT(made, 1000u); // it got a long way before the budget said no
  EXPECT_GE(grcore_context_memory_refusals(w.ctx), 1u);
  uint64_t live = w.stats().live_objects;
  EXPECT_EQ(live, made);
  // Nothing was lost, and releasing the chain lets allocation go on.
  void * none = reinterpret_cast<void *>(0x1);
  EXPECT_EQ(grheap_alloc(w.heap, &kPlainNode, &none), GRHEAP_ERR_LIMIT);
  EXPECT_EQ(none, reinterpret_cast<void *>(0x1));
  head = nullptr;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(grheap_alloc(w.heap, &kPlainNode, &none), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &head), GRHEAP_OK);
}

TEST_F(FailTest, TheBudgetRefusingAHandleOrAPinIsLimitToo) {
  WorldConfig config;
  config.memory = 300000;
  config.reserve = 0;
  HeapWorld w(config);
  Node * n = new_node(w.heap, 1);
  // Use up everything the budget has left with raw allocations, then ask.
  const GRCORE_Allocator * a = grcore_context_allocator(w.ctx);
  std::vector<void *> hog;
  while (true) {
    void * p = a->malloc_fn(a->ctx, 16);
    if (p == nullptr) {
      break;
    }
    hog.push_back(p);
  }
  GRHEAP_Handle * h = nullptr;
  EXPECT_EQ(grheap_handle_create(w.heap, n, &h), GRHEAP_ERR_LIMIT);
  EXPECT_EQ(h, nullptr);
  EXPECT_EQ(grheap_pin(w.heap, n), GRHEAP_ERR_LIMIT);
  EXPECT_EQ(w.stats().handles, 0u);
  EXPECT_EQ(w.stats().pins, 0u);
  for (void * p : hog) {
    a->free_fn(a->ctx, p);
  }
  EXPECT_EQ(grheap_handle_create(w.heap, n, &h), GRHEAP_OK);
}

TEST_F(FailTest, ThePageProviderRefusingIsOomEvenWithABudgetSetAndRoomInIt) {
  WorldConfig config;
  config.memory = 10000000;
  HeapWorld w(config);
  w.pages.fail = true;
  void * p;
  EXPECT_EQ(grheap_alloc(w.heap, &kNode, &p), GRHEAP_ERR_OOM);
}

/* ---- Reclaim at the poll (CAP-2, AD-21) -------------------------------- */

namespace {

/* Allocates garbage until the context is over its budget. */
void exceed_budget(HeapWorld & w, uint64_t limit) {
  while (grcore_context_memory_in_use(w.ctx) <= limit) {
    void * p;
    ASSERT_EQ(grheap_alloc(w.heap, &kPlainNode, &p), GRHEAP_OK);
  }
  ASSERT_TRUE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_MEMORY));
}

} // namespace

TEST_F(FailTest, APollOverBudgetCollectsBeforeTheVerdictAndTheRunContinues) {
  WorldConfig config;
  config.memory = 100000;
  config.reserve = 400000; // room to go over; the poll is what answers
  config.auto_collect = false;
  HeapWorld w(config);
  exceed_budget(w, 100000);
  ASSERT_EQ(w.stats().collections, 0u);
  GRCORE_Outcome outcome;
  std::vector<GRCORE_Verdict> verdicts;
  Fn fn{[&](GRCORE_Context * c) {
    for (int i = 0; i < 4; i++) {
      verdicts.push_back(GRCORE_POLL(c));
    }
    return GRCORE_STEP_FINISHED;
  }};
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(w.stats().collections, 1u);
  EXPECT_EQ(verdicts,
      std::vector<GRCORE_Verdict>(4, GRCORE_VERDICT_CONTINUE));
  EXPECT_LE(grcore_context_memory_in_use(w.ctx), 100000u);
  EXPECT_FALSE(grcore_context_request_pending(w.ctx, GRCORE_REQUEST_MEMORY));
  EXPECT_EQ(w.stats().live_objects, 0u);
}

TEST_F(FailTest, APollOverBudgetWithNothingToReclaimCollectsOnceThenPauses) {
  WorldConfig config;
  config.memory = 100000;
  config.reserve = 400000;
  config.auto_collect = false;
  HeapWorld w(config);
  void * head = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &head), GRHEAP_OK);
  while (grcore_context_memory_in_use(w.ctx) <= 100000) {
    void * p;
    ASSERT_EQ(grheap_alloc(w.heap, &kPlainNode, &p), GRHEAP_OK);
    static_cast<Node *>(p)->a = head;
    head = p;
  }
  GRCORE_Outcome outcome;
  Fn fn{[&](GRCORE_Context * c) {
    for (int i = 0; i < 4; i++) {
      GRCORE_Verdict v = GRCORE_POLL(c);
      if (v == GRCORE_VERDICT_PAUSE) {
        return GRCORE_STEP_PAUSED;
      }
    }
    return GRCORE_STEP_FINISHED;
  }};
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  // The first poll reclaimed what it could (nothing) and the next one voted.
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_EQ(w.stats().collections, 1u);
  EXPECT_EQ(grcore_context_pause_key_count(w.ctx), 1u);
  EXPECT_EQ(grcore_context_pause_key(w.ctx, 0),
      grcore_core_key(GRCORE_REQUEST_MEMORY));
  // The host raises the budget and the run finishes.
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, 100000000), GRCORE_OK);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  ASSERT_EQ(grheap_root_remove(w.heap, &head), GRHEAP_OK);
}

TEST_F(FailTest, AnArenaHeapIgnoresAReclaimRequest) {
  WorldConfig config;
  config.mode = GRHEAP_MODE_ARENA;
  config.memory = 100000;
  config.reserve = 400000;
  HeapWorld w(config);
  exceed_budget(w, 100000);
  GRCORE_Outcome outcome;
  Fn fn{[&](GRCORE_Context * c) {
    for (int i = 0; i < 3; i++) {
      if (GRCORE_POLL(c) == GRCORE_VERDICT_PAUSE) {
        return GRCORE_STEP_PAUSED;
      }
    }
    return GRCORE_STEP_FINISHED;
  }};
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(w.stats().collections, 0u);
  EXPECT_GT(w.stats().live_objects, 0u);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED); // nothing was given back
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
