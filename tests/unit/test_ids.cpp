/**
 * @file
 *
 * Stable IDs and the identity hash.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <set>
#include <thread>

namespace {

GRHEAP_Handle * hold(HeapWorld & w, void * p) {
  GRHEAP_Handle * h = nullptr;
  EXPECT_EQ(grheap_handle_create(w.heap, p, &h), GRHEAP_OK);
  return h;
}

} // namespace

class IdsTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

TEST_F(IdsTest, TheSameObjectGetsTheSameIdAndIdsIncreaseFromOne) {
  HeapWorld w;
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * ha = hold(w, a);
  Node * b = new_node(w.heap, 2);
  GRHEAP_Handle * hb = hold(w, b);
  uint64_t ida = 0, ida2 = 0, idb = 0;
  ASSERT_EQ(grheap_id_of(w.heap, a, &ida), GRHEAP_OK);
  EXPECT_EQ(ida, 1u);
  ASSERT_EQ(grheap_id_of(w.heap, a, &ida2), GRHEAP_OK);
  EXPECT_EQ(ida2, ida);
  ASSERT_EQ(grheap_id_of(w.heap, b, &idb), GRHEAP_OK);
  EXPECT_EQ(idb, 2u);
  EXPECT_EQ(w.stats().ids_issued, 2u);
  EXPECT_EQ(w.stats().ids_live, 2u);
  ASSERT_EQ(grheap_handle_destroy(hb), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(ha), GRHEAP_OK);
}

TEST_F(IdsTest, AnIdSurvivesCollectionsWhileItsObjectLivesAndResolvesBack) {
  HeapWorld w;
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * ha = hold(w, a);
  uint64_t id = 0;
  ASSERT_EQ(grheap_id_of(w.heap, a, &id), GRHEAP_OK);
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    uint64_t again = 0;
    ASSERT_EQ(grheap_id_of(w.heap, a, &again), GRHEAP_OK);
    EXPECT_EQ(again, id);
    void * back = nullptr;
    ASSERT_EQ(grheap_object_of_id(w.heap, id, &back), GRHEAP_OK);
    EXPECT_EQ(back, a);
  }
  ASSERT_EQ(grheap_handle_destroy(ha), GRHEAP_OK);
}

TEST_F(IdsTest, TheIdOfAFreedObjectIsUnknownAndIsNeverReused) {
  HeapWorld w;
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * ha = hold(w, a);
  uint64_t id = 0;
  ASSERT_EQ(grheap_id_of(w.heap, a, &id), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(ha), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(destroyed(1));
  void * back = reinterpret_cast<void *>(0x1);
  EXPECT_EQ(grheap_object_of_id(w.heap, id, &back), GRHEAP_ERR_INVALID);
  EXPECT_EQ(back, reinterpret_cast<void *>(0x1)); // untouched
  EXPECT_EQ(w.stats().ids_live, 0u);
  EXPECT_EQ(w.stats().ids_issued, 1u);
  // A new object that lands where the old one was gets a new ID.
  Node * b = new_node(w.heap, 2);
  GRHEAP_Handle * hb = hold(w, b);
  uint64_t idb = 0;
  ASSERT_EQ(grheap_id_of(w.heap, b, &idb), GRHEAP_OK);
  EXPECT_GT(idb, id);
  EXPECT_EQ(grheap_object_of_id(w.heap, id, &back), GRHEAP_ERR_INVALID);
  ASSERT_EQ(grheap_handle_destroy(hb), GRHEAP_OK);
}

TEST_F(IdsTest, TheIdIsForgottenBeforeTheDestroyHookRuns) {
  HeapWorld w(steady());
  static GRHEAP_Heap * the_heap;
  static uint64_t watched;
  static GRHEAP_Result seen;
  the_heap = w.heap;
  seen = GRHEAP_OK;
  static const GRHEAP_Type spy = GRHEAP_TYPE_INIT("spy", 16, nullptr,
      [](void *) {
        void * out;
        seen = grheap_object_of_id(the_heap, watched, &out);
      },
      nullptr, nullptr, nullptr);
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &spy, &p), GRHEAP_OK);
  ASSERT_EQ(grheap_id_of(w.heap, p, &watched), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(seen, GRHEAP_ERR_INVALID);
}

TEST_F(IdsTest, TheIdentityHashIsAFunctionOfTheIdAndStable) {
  HeapWorld w;
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * ha = hold(w, a);
  Node * b = new_node(w.heap, 2);
  GRHEAP_Handle * hb = hold(w, b);
  uint64_t ha1 = 0, ha2 = 0, hb1 = 0;
  ASSERT_EQ(grheap_identity_hash(w.heap, a, &ha1), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  ASSERT_EQ(grheap_identity_hash(w.heap, a, &ha2), GRHEAP_OK);
  EXPECT_EQ(ha1, ha2);
  ASSERT_EQ(grheap_identity_hash(w.heap, b, &hb1), GRHEAP_OK);
  EXPECT_NE(ha1, hb1);
  // Asking for the hash assigned an ID.
  EXPECT_EQ(w.stats().ids_issued, 2u);
  uint64_t ida = 0;
  ASSERT_EQ(grheap_id_of(w.heap, a, &ida), GRHEAP_OK);
  EXPECT_EQ(w.stats().ids_issued, 2u);
  // Many objects: the hashes are well spread (distinct, in particular).
  std::set<uint64_t> seen;
  std::vector<GRHEAP_Handle *> hs;
  for (int i = 0; i < 500; i++) {
    Node * n = new_node(w.heap, static_cast<uint64_t>(100 + i));
    hs.push_back(hold(w, n));
    uint64_t h = 0;
    ASSERT_EQ(grheap_identity_hash(w.heap, n, &h), GRHEAP_OK);
    seen.insert(h);
  }
  EXPECT_EQ(seen.size(), 500u);
  for (auto * h : hs) {
    ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
  }
  ASSERT_EQ(grheap_handle_destroy(hb), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(ha), GRHEAP_OK);
}

TEST_F(IdsTest, ManyIdsAcrossGrowthAndCompactionStayConsistent) {
  HeapWorld w;
  constexpr int kN = 3000;
  std::vector<GRHEAP_Handle *> hs(kN, nullptr);
  std::vector<Node *> objs(kN, nullptr);
  std::vector<uint64_t> ids(kN, 0);
  for (int i = 0; i < kN; i++) {
    objs[i] = new_node(w.heap, static_cast<uint64_t>(i));
    hs[i] = hold(w, objs[i]);
    ASSERT_EQ(grheap_id_of(w.heap, objs[i], &ids[i]), GRHEAP_OK);
  }
  // Drop every third, collect (compacts the table), and check all the rest.
  for (int i = 0; i < kN; i += 3) {
    ASSERT_EQ(grheap_handle_destroy(hs[i]), GRHEAP_OK);
    hs[i] = nullptr;
  }
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  for (int i = 0; i < kN; i++) {
    void * back = nullptr;
    if (i % 3 == 0) {
      EXPECT_EQ(grheap_object_of_id(w.heap, ids[i], &back), GRHEAP_ERR_INVALID);
    } else {
      ASSERT_EQ(grheap_object_of_id(w.heap, ids[i], &back), GRHEAP_OK) << i;
      EXPECT_EQ(back, objs[i]);
      uint64_t again = 0;
      ASSERT_EQ(grheap_id_of(w.heap, objs[i], &again), GRHEAP_OK);
      EXPECT_EQ(again, ids[i]);
    }
  }
  EXPECT_EQ(w.stats().ids_live, static_cast<uint64_t>(kN - kN / 3));
  for (int i = 0; i < kN; i++) {
    if (hs[i] != nullptr) {
      ASSERT_EQ(grheap_handle_destroy(hs[i]), GRHEAP_OK);
    }
  }
}

TEST_F(IdsTest, WhatIsNotALivePayloadStartOrNotUsableIsRefusedAndWritesNothing) {
  HeapWorld w;
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * ha = hold(w, a);
  uint64_t out = 777;
  void * obj = reinterpret_cast<void *>(0x1);
  EXPECT_EQ(grheap_id_of(nullptr, a, &out), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_id_of(w.heap, nullptr, &out), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_id_of(w.heap, a, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_id_of(w.heap, reinterpret_cast<char *>(a) + 8, &out),
      GRHEAP_ERR_INVALID);
  int local;
  EXPECT_EQ(grheap_id_of(w.heap, &local, &out), GRHEAP_ERR_INVALID);
  EXPECT_EQ(out, 777u);
  EXPECT_EQ(grheap_identity_hash(w.heap, nullptr, &out), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_identity_hash(w.heap, a, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_identity_hash(w.heap, &local, &out), GRHEAP_ERR_INVALID);
  EXPECT_EQ(out, 777u);
  EXPECT_EQ(grheap_object_of_id(nullptr, 1, &obj), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_object_of_id(w.heap, 1, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_object_of_id(w.heap, 0, &obj), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_object_of_id(w.heap, 1, &obj), GRHEAP_ERR_INVALID); // none yet
  EXPECT_EQ(grheap_object_of_id(w.heap, UINT64_MAX, &obj), GRHEAP_ERR_INVALID);
  EXPECT_EQ(obj, reinterpret_cast<void *>(0x1));
  EXPECT_EQ(w.stats().ids_issued, 0u);
  // Another thread is not the owner.
  std::thread other([&] {
    EXPECT_EQ(grheap_id_of(w.heap, a, &out), GRHEAP_ERR_INVALID);
    EXPECT_EQ(grheap_identity_hash(w.heap, a, &out), GRHEAP_ERR_INVALID);
    EXPECT_EQ(grheap_object_of_id(w.heap, 1, &obj), GRHEAP_ERR_INVALID);
  });
  other.join();
  EXPECT_EQ(out, 777u);
  ASSERT_EQ(grheap_handle_destroy(ha), GRHEAP_OK);
}

TEST_F(IdsTest, IssuingFromInsideADestroyHookIsRefusedAndAQueryIsNot) {
  HeapWorld w(steady());
  static GRHEAP_Heap * the_heap;
  static void * victim;
  static GRHEAP_Result issued;
  static GRHEAP_Result queried;
  the_heap = w.heap;
  static const GRHEAP_Type probe = GRHEAP_TYPE_INIT("probe", 16, nullptr,
      [](void *) {
        uint64_t id;
        issued = grheap_id_of(the_heap, victim, &id);
        void * out;
        queried = grheap_object_of_id(the_heap, 1, &out);
      },
      nullptr, nullptr, nullptr);
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &probe, &p), GRHEAP_OK);
  Node * other = new_node(w.heap, 1);
  GRHEAP_Handle * ho = hold(w, other);
  victim = other;
  uint64_t id;
  ASSERT_EQ(grheap_id_of(w.heap, other, &id), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(issued, GRHEAP_ERR_INVALID);
  EXPECT_EQ(queried, GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(ho), GRHEAP_OK);
}

TEST_F(IdsTest, AFailedTableGrowthIssuesNoIdAndStartsNoCollection) {
  HeapWorld w(steady());
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * ha = hold(w, a);
  GRHEAP_Stats before = w.stats();
  w.allocator.fail_all = true;
  uint64_t id = 55;
  EXPECT_EQ(grheap_id_of(w.heap, a, &id), GRHEAP_ERR_OOM);
  uint64_t h = 55;
  EXPECT_EQ(grheap_identity_hash(w.heap, a, &h), GRHEAP_ERR_OOM);
  w.allocator.fail_all = false;
  EXPECT_EQ(id, 55u);
  EXPECT_EQ(h, 55u);
  GRHEAP_Stats after = w.stats();
  EXPECT_EQ(after.ids_issued, before.ids_issued);
  EXPECT_EQ(after.ids_live, before.ids_live);
  EXPECT_EQ(after.collections, before.collections);
  // The next success gets the first number: the refusals consumed none.
  ASSERT_EQ(grheap_id_of(w.heap, a, &id), GRHEAP_OK);
  EXPECT_EQ(id, 1u);
  ASSERT_EQ(grheap_handle_destroy(ha), GRHEAP_OK);
}

TEST_F(IdsTest, EveryNthAllocatorFailureWhileIssuingIsSurvived) {
  // Two allocator calls per growth, at the 1st, 17th and 33rd ID: six in all.
  for (long n = 1; n <= 6; n++) {
    HeapWorld w(steady());
    constexpr int kN = 40;
    std::vector<Node *> objs;
    std::vector<GRHEAP_Handle *> hs;
    for (int i = 0; i < kN; i++) {
      objs.push_back(new_node(w.heap, static_cast<uint64_t>(i)));
      hs.push_back(hold(w, objs.back()));
    }
    w.allocator.calls = 0;
    w.allocator.fail_at = n;
    uint64_t last = 0;
    int failures = 0;
    for (int i = 0; i < kN; i++) {
      uint64_t id = 0;
      GRHEAP_Result r = grheap_id_of(w.heap, objs[i], &id);
      if (r != GRHEAP_OK) {
        EXPECT_EQ(r, GRHEAP_ERR_OOM) << "N=" << n;
        EXPECT_EQ(id, 0u);
        failures++;
        r = grheap_id_of(w.heap, objs[i], &id); // fail_at was a single call
      }
      ASSERT_EQ(r, GRHEAP_OK);
      EXPECT_GT(id, last); // monotonic, nothing skipped or repeated
      last = id;
    }
    w.allocator.fail_at = 0;
    EXPECT_EQ(last, static_cast<uint64_t>(kN)) << "N=" << n;
    EXPECT_EQ(failures, 1) << "N=" << n;
    for (auto * h : hs) {
      ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
    }
  }
}

TEST_F(IdsTest, TheBudgetRefusingTableSpaceIsLimit) {
  WorldConfig config;
  config.reserve = 0;
  HeapWorld w(steady(config));
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * ha = hold(w, a);
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx,
                grcore_context_memory_in_use(w.ctx)),
      GRCORE_OK);
  uint64_t id = 0;
  EXPECT_EQ(grheap_id_of(w.heap, a, &id), GRHEAP_ERR_LIMIT);
  EXPECT_EQ(w.stats().ids_issued, 0u);
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  EXPECT_EQ(grheap_id_of(w.heap, a, &id), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(ha), GRHEAP_OK);
}

TEST_F(IdsTest, AnArenaIssuesIdsAndResetEmptiesTheTableButNotTheNumbering) {
  WorldConfig config;
  config.mode = GRHEAP_MODE_ARENA;
  HeapWorld w(config);
  Node * a = new_node(w.heap, 1);
  uint64_t id = 0;
  ASSERT_EQ(grheap_id_of(w.heap, a, &id), GRHEAP_OK);
  EXPECT_EQ(id, 1u);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK); // does nothing
  void * back = nullptr;
  ASSERT_EQ(grheap_object_of_id(w.heap, id, &back), GRHEAP_OK);
  EXPECT_EQ(back, a);
  ASSERT_EQ(grheap_arena_reset(w.heap), GRHEAP_OK);
  EXPECT_EQ(grheap_object_of_id(w.heap, id, &back), GRHEAP_ERR_INVALID);
  EXPECT_EQ(w.stats().ids_live, 0u);
  Node * b = new_node(w.heap, 2);
  uint64_t idb = 0;
  ASSERT_EQ(grheap_id_of(w.heap, b, &idb), GRHEAP_OK);
  EXPECT_EQ(idb, 2u); // numbering carries on
}

TEST_F(IdsTest, TheTableMigratesWithTheContext) {
  HeapWorld w;
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * ha = hold(w, a);
  uint64_t id = 0;
  ASSERT_EQ(grheap_id_of(w.heap, a, &id), GRHEAP_OK);
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  uint64_t seen = 0;
  uint64_t fresh = 0;
  std::thread other([&] {
    ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    ASSERT_EQ(grheap_id_of(w.heap, a, &seen), GRHEAP_OK);
    Node * b = new_node(w.heap, 2);
    GRHEAP_Handle * hb = hold(w, b);
    ASSERT_EQ(grheap_id_of(w.heap, b, &fresh), GRHEAP_OK);
    ASSERT_EQ(grheap_handle_destroy(hb), GRHEAP_OK);
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  });
  other.join();
  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  EXPECT_EQ(seen, id);
  EXPECT_EQ(fresh, id + 1u);
  void * back = nullptr;
  EXPECT_EQ(grheap_object_of_id(w.heap, fresh, &back), GRHEAP_ERR_INVALID);
  ASSERT_EQ(grheap_handle_destroy(ha), GRHEAP_OK);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
