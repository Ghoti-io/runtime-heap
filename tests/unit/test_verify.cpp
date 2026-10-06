/**
 * @file
 *
 * Barrier-verify: a shadow of every store, compared with the slots, so that a
 * pointer written without `grheap_store` is found.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>
#include <thread>

namespace {

WorldConfig verified(GRHEAP_Verify mode = GRHEAP_VERIFY_COUNT) {
  WorldConfig c;
  c.torture = 0; // these tests hold objects in C variables
  c.verify = mode;
  return c;
}

/* An object whose only traced slot is a word. */
struct Boxed {
  uint64_t word;
};
void boxed_trace(GRHEAP_Tracer * t, void * payload) {
  grheap_trace_word(t, &static_cast<Boxed *>(payload)->word);
}
const GRHEAP_Type kBoxed = GRHEAP_TYPE_INIT("boxed", sizeof(Boxed), boxed_trace, nullptr, nullptr, nullptr, nullptr);

/* 48 bytes, but only the first slot is traced; the rest is data. */
struct Wide {
  void * traced;
  void * data[4];
};
void wide_trace(GRHEAP_Tracer * t, void * payload) {
  grheap_trace_slot(t, &static_cast<Wide *>(payload)->traced);
}
const GRHEAP_Type kWide = GRHEAP_TYPE_INIT("wide", sizeof(Wide), wide_trace, nullptr, nullptr, nullptr, nullptr);

uint64_t violations_of(HeapWorld & w) {
  uint64_t v = 12345;
  EXPECT_EQ(grheap_verify(w.heap, &v), GRHEAP_OK);
  return v;
}

} // namespace

class VerifyTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

TEST_F(VerifyTest, AStoreThroughTheFunctionsIsClean) {
  HeapWorld w(verified());
  Node * a = new_node(w.heap, 1);
  Node * b = new_node(w.heap, 2);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, a, &h), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, a, &a->a, b), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, a, &a->b, a), GRHEAP_OK);
  void * src[2] = {a, b};
  void * pair[2] = {};
  (void)pair;
  EXPECT_EQ(violations_of(w), 0u);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().verify_violations, 0u);
  (void)src;
}

TEST_F(VerifyTest, APointerWrittenDirectlyIsFoundByVerifyAndByTheNextCollection) {
  HeapWorld w(unmoved(verified()));
  Node * a = new_node(w.heap, 1);
  Node * b = new_node(w.heap, 2);
  GRHEAP_Handle * ha;
  GRHEAP_Handle * hb;
  ASSERT_EQ(grheap_handle_create(w.heap, a, &ha), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_create(w.heap, b, &hb), GRHEAP_OK);
  a->a = b; // the bypass
  EXPECT_EQ(violations_of(w), 1u);
  EXPECT_EQ(w.stats().verify_violations, 1u);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK); // finds it again, at its start
  EXPECT_EQ(w.stats().verify_violations, 2u);
  // Repaired through the store: clean.
  ASSERT_EQ(grheap_store(w.heap, a, &a->a, b), GRHEAP_OK);
  EXPECT_EQ(violations_of(w), 0u);
  // A second bypass, of a slot that had been stored to: the value differs.
  a->a = a;
  EXPECT_EQ(violations_of(w), 1u);
  a->a = b; // writing back what the shadow holds is no violation
  EXPECT_EQ(violations_of(w), 0u);
}

TEST_F(VerifyTest, AWordWrittenDirectlyIsFoundToo) {
  WorldConfig config = verified();
  config.codec_set = true;
  config.codec = GRHEAP_ValueCodec{1, 1, ~UINT64_C(1), 1, 0};
  HeapWorld w(config);
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &kBoxed, &p), GRHEAP_OK);
  auto * box = static_cast<Boxed *>(p);
  ASSERT_EQ(grheap_store_word(w.heap, box, &box->word, 5u << 1), GRHEAP_OK);
  EXPECT_EQ(violations_of(w), 0u);
  box->word = 9u << 1;
  EXPECT_EQ(violations_of(w), 1u);
}

TEST_F(VerifyTest, ARangeStoreIsRecordedSlotBySlot) {
  HeapWorld w(verified());
  Node * a = new_node(w.heap, 1);
  Node * b = new_node(w.heap, 2);
  void * src[2] = {b, a};
  ASSERT_EQ(grheap_store_range(w.heap, a, &a->a, src, 2), GRHEAP_OK);
  EXPECT_EQ(a->a, b);
  EXPECT_EQ(a->b, a);
  EXPECT_EQ(violations_of(w), 0u);
  a->b = b; // one slot of the range, bypassed
  EXPECT_EQ(violations_of(w), 1u);
  a->b = a; // back to what the shadow holds
  EXPECT_EQ(violations_of(w), 0u);
  // An overlapping range store (memmove) keeps its result and stays clean.
  ASSERT_EQ(grheap_store_range(w.heap, a, &a->a, &a->b, 1), GRHEAP_OK);
  EXPECT_EQ(a->a, a);
  EXPECT_EQ(violations_of(w), 0u);
}

TEST_F(VerifyTest, StoringNullForgetsTheSlotAndAnUntouchedSlotIsZero) {
  HeapWorld w(verified());
  Node * a = new_node(w.heap, 1);
  ASSERT_EQ(grheap_store(w.heap, a, &a->a, a), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, a, &a->a, nullptr), GRHEAP_OK);
  EXPECT_EQ(violations_of(w), 0u);
  a->a = a; // the shadow says zero
  EXPECT_EQ(violations_of(w), 1u);
}

TEST_F(VerifyTest, OnlyTracedSlotsAreCompared) {
  HeapWorld w(verified());
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &kWide, &p), GRHEAP_OK);
  auto * wide = static_cast<Wide *>(p);
  wide->data[2] = wide; // not a traced slot: nobody promised a barrier here
  EXPECT_EQ(violations_of(w), 0u);
  wide->traced = wide;
  EXPECT_EQ(violations_of(w), 1u);
}

TEST_F(VerifyTest, ABadHolderIsAViolationThatStillStores) {
  HeapWorld w(verified());
  Node * a = new_node(w.heap, 1);
  Node * b = new_node(w.heap, 2);
  // The holder's cell does not contain the slot.
  ASSERT_EQ(grheap_store(w.heap, b, &a->a, b), GRHEAP_OK);
  EXPECT_EQ(a->a, b); // the store happened
  EXPECT_EQ(w.stats().verify_violations, 1u);
  // A holder that is not a heap object at all.
  int local;
  ASSERT_EQ(grheap_store(w.heap, &local, &a->b, a), GRHEAP_OK);
  EXPECT_EQ(a->b, a);
  EXPECT_EQ(w.stats().verify_violations, 2u);
  // A NULL holder for a slot inside a heap object.
  ASSERT_EQ(grheap_store(w.heap, nullptr, &b->a, a), GRHEAP_OK);
  EXPECT_EQ(b->a, a);
  EXPECT_EQ(w.stats().verify_violations, 3u);
  // A NULL holder for a root slot or a stack variable is exactly right.
  void * stack_slot = nullptr;
  ASSERT_EQ(grheap_store(w.heap, nullptr, &stack_slot, a), GRHEAP_OK);
  EXPECT_EQ(stack_slot, a);
  EXPECT_EQ(w.stats().verify_violations, 3u);
  // A correct store counts nothing.
  ASSERT_EQ(grheap_store(w.heap, a, &a->a, b), GRHEAP_OK);
  EXPECT_EQ(w.stats().verify_violations, 3u);
  // A range into one object named under another.
  void * src[2] = {a, b};
  uint64_t before = w.stats().verify_violations;
  ASSERT_EQ(grheap_store_range(w.heap, a, &b->a, src, 2), GRHEAP_OK);
  EXPECT_GT(w.stats().verify_violations, before);
}

TEST_F(VerifyTest, ADeadObjectsShadowIsForgottenSoItsCellCanBeReused) {
  HeapWorld w(verified());
  Node * keep = new_node(w.heap, 1); // keeps the block alive
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, keep, &h), GRHEAP_OK);
  Node * dead = new_node(w.heap, 2);
  ASSERT_EQ(grheap_store(w.heap, dead, &dead->a, keep), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, dead, &dead->b, dead), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(destroyed(2));
  Node * again = new_node(w.heap, 3);
  EXPECT_EQ(again, dead); // the same cell, zero-filled
  EXPECT_EQ(violations_of(w), 0u);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().verify_violations, 0u);
}

TEST_F(VerifyTest, AStoreToAnUntracedSlotIsForgottenWithItsObjectToo) {
  HeapWorld w(verified());
  Node * keep = new_node(w.heap, 1);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, keep, &h), GRHEAP_OK);
  // A Wide is 40 bytes, a Node 24: different cells. Use a Wide twice instead:
  // a store into its data slot (untraced) with the right holder is recorded.
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &kWide, &p), GRHEAP_OK);
  auto * wide = static_cast<Wide *>(p);
  ASSERT_EQ(grheap_store(w.heap, wide, &wide->data[1], keep), GRHEAP_OK);
  EXPECT_EQ(violations_of(w), 0u);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK); // wide is garbage
  // The cell comes back as a type whose traced slot lies where that data was.
  static const GRHEAP_Type kOver = GRHEAP_TYPE_INIT("over", sizeof(Wide),
      [](GRHEAP_Tracer * t, void * q) {
        grheap_trace_slot(t, &static_cast<Wide *>(q)->data[1]);
      },
      nullptr, nullptr, nullptr, nullptr);
  void * q;
  ASSERT_EQ(grheap_alloc(w.heap, &kOver, &q), GRHEAP_OK);
  EXPECT_EQ(q, p);
  EXPECT_EQ(violations_of(w), 0u);
}

namespace {

/* Slots that are not in any heap object: a buffer a dying object owns and
 * whose memory something else then reuses, zeroed. */
void * g_buffer[4];

struct Owner {
  uint64_t id;
};
void owner_trace(GRHEAP_Tracer * t, void * payload) {
  (void)payload;
  grheap_trace_slot(t, &g_buffer[0]);
  grheap_trace_slot(t, &g_buffer[1]);
}
void owner_destroy(void *) { std::memset(g_buffer, 0, sizeof g_buffer); }
const GRHEAP_Type kOwner = GRHEAP_TYPE_INIT("owner", sizeof(Owner), owner_trace, owner_destroy, nullptr, nullptr, nullptr);

} // namespace

TEST_F(VerifyTest, ADeadObjectsShadowOutsideItsCellIsForgottenWithIt) {
  // The object's trace reports slots in a buffer it owns, outside its cell. A
  // store into one with the object as holder is itself a violation ("the slot
  // is not inside the holder") and is counted when it happens; what must not
  // happen is a second, false violation later, when the object is gone, the
  // buffer is zeroed and reused, and another object reports the same address.
  HeapWorld w(verified());
  std::memset(g_buffer, 0, sizeof g_buffer);
  Node * keep = new_node(w.heap, 1);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, keep, &h), GRHEAP_OK);
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &kOwner, &p), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, p, &g_buffer[0], keep), GRHEAP_OK);
  uint64_t at_store = w.stats().verify_violations;
  EXPECT_GE(at_store, 1u) << "the store named a holder that does not contain the slot";
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK); // the owner is garbage; its destroy zeroes the buffer
  EXPECT_FALSE(grheap_contains(w.heap, p));
  // A new live object reports the same slots, which now hold zero.
  void * q;
  ASSERT_EQ(grheap_alloc(w.heap, &kOwner, &q), GRHEAP_OK);
  GRHEAP_Handle * hq;
  ASSERT_EQ(grheap_handle_create(w.heap, q, &hq), GRHEAP_OK);
  uint64_t before = w.stats().verify_violations;
  EXPECT_EQ(violations_of(w), 0u) << "a stale entry for a dead object's slot was compared";
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().verify_violations, before);
  ASSERT_EQ(grheap_handle_destroy(hq), GRHEAP_OK);
}

TEST_F(VerifyTest, VerifyOffRecordsNothingAndFindsNothing) {
  HeapWorld w(verified(GRHEAP_VERIFY_OFF));
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, a, &h), GRHEAP_OK);
  a->a = a;
  EXPECT_EQ(violations_of(w), 0u);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().verify_violations, 0u);
  // Stores with a bad holder are not checked either.
  ASSERT_EQ(grheap_store(w.heap, nullptr, &a->b, a), GRHEAP_OK);
  EXPECT_EQ(w.stats().verify_violations, 0u);
}

TEST_F(VerifyTest, AbortModeNamesTheTypeAndTheSlotAndAborts) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_DEATH(
      {
        HeapWorld w(verified(GRHEAP_VERIFY_ABORT));
        Node * a = new_node(w.heap, 1);
        a->a = a;
        uint64_t v;
        (void)grheap_verify(w.heap, &v);
      },
      "barrier-verify.*\"node\"");
  EXPECT_DEATH(
      {
        HeapWorld w(verified(GRHEAP_VERIFY_ABORT));
        Node * a = new_node(w.heap, 1);
        (void)grheap_store(w.heap, nullptr, &a->a, a);
      },
      "barrier-verify.*NULL holder");
  // And a clean program does not abort.
  HeapWorld w(verified(GRHEAP_VERIFY_ABORT));
  Node * a = new_node(w.heap, 1);
  ASSERT_EQ(grheap_store(w.heap, a, &a->a, a), GRHEAP_OK);
  EXPECT_EQ(violations_of(w), 0u);
}

TEST_F(VerifyTest, AnArenaIsWalkedToo) {
  WorldConfig config = verified();
  config.mode = GRHEAP_MODE_ARENA;
  HeapWorld w(config);
  Node * a = new_node(w.heap, 1);
  Node * b = new_node(w.heap, 2);
  ASSERT_EQ(grheap_store(w.heap, a, &a->a, b), GRHEAP_OK);
  EXPECT_EQ(violations_of(w), 0u);
  b->a = a;
  EXPECT_EQ(violations_of(w), 1u);
  // And a bad holder in an arena: found by locating the object around the slot.
  ASSERT_EQ(grheap_store(w.heap, nullptr, &a->b, a), GRHEAP_OK);
  EXPECT_GE(w.stats().verify_violations, 2u);
  ASSERT_EQ(grheap_arena_reset(w.heap), GRHEAP_OK);
  EXPECT_EQ(violations_of(w), 0u); // nothing left to find
}

TEST_F(VerifyTest, ItChangesNoResultOfTheHeap) {
  struct Outcome {
    GRHEAP_Stats stats;
    std::vector<uint64_t> destroyed;
    std::vector<uint64_t> alive;
  };
  auto run = [](GRHEAP_Verify mode) {
    WorldConfig config;
    config.torture = 0;
    config.verify = mode;
    config.threshold = 20000;
    HeapWorld w(config);
    destroyed_log().clear();
    void * roots[3] = {};
    for (auto & r : roots) {
      EXPECT_EQ(grheap_root_add(w.heap, &r), GRHEAP_OK);
    }
    uint64_t seed = 99;
    for (int i = 0; i < 3000; i++) {
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      Node * n = new_node(w.heap, static_cast<uint64_t>(i));
      void ** slot = &roots[(seed >> 33) % 3];
      EXPECT_EQ(grheap_store(w.heap, n, &n->a, *slot), GRHEAP_OK);
      EXPECT_EQ(grheap_store(w.heap, n, &n->b, roots[(seed >> 45) % 3]),
          GRHEAP_OK);
      *slot = n;
      if ((seed >> 40) % 7 == 0) {
        *slot = nullptr;
      }
    }
    EXPECT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    Outcome o;
    o.stats = w.stats();
    o.destroyed = destroyed_log();
    for (auto * r : roots) {
      for (auto * n = static_cast<Node *>(r); n != nullptr;
          n = static_cast<Node *>(n->a)) {
        o.alive.push_back(n->id);
      }
    }
    for (auto & r : roots) {
      EXPECT_EQ(grheap_root_remove(w.heap, &r), GRHEAP_OK);
    }
    EXPECT_EQ(o.stats.verify_violations, 0u);
    return o;
  };
  Outcome off = run(GRHEAP_VERIFY_OFF);
  Outcome on = run(GRHEAP_VERIFY_COUNT);
  EXPECT_EQ(off.alive, on.alive);
  EXPECT_EQ(off.destroyed, on.destroyed);
  EXPECT_EQ(off.stats.live_objects, on.stats.live_objects);
  EXPECT_EQ(off.stats.live_bytes, on.stats.live_bytes);
  EXPECT_EQ(off.stats.collections, on.stats.collections);
  EXPECT_EQ(off.stats.allocations, on.stats.allocations);
  EXPECT_EQ(off.stats.frees, on.stats.frees);
  EXPECT_EQ(off.stats.stores, on.stats.stores);
  EXPECT_EQ(off.stats.mapped_bytes, on.stats.mapped_bytes);
  EXPECT_EQ(off.stats.invalid_pointers, on.stats.invalid_pointers);
  EXPECT_GE(on.stats.collections, 2u); // collections ran with verify on
}

TEST_F(VerifyTest, TheShadowSurvivesManyRandomStoresAndCollections) {
  HeapWorld w(verified());
  constexpr int kN = 600;
  std::vector<Node *> nodes;
  std::vector<GRHEAP_Handle *> hs;
  for (int i = 0; i < kN; i++) {
    Node * n = new_node(w.heap, static_cast<uint64_t>(i));
    nodes.push_back(n);
    GRHEAP_Handle * h;
    ASSERT_EQ(grheap_handle_create(w.heap, n, &h), GRHEAP_OK);
    hs.push_back(h);
  }
  uint64_t seed = 7;
  for (int step = 0; step < 20000; step++) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    Node * holder = nodes[(seed >> 33) % kN];
    void * value = (seed >> 20) % 4 == 0 ? nullptr : nodes[(seed >> 45) % kN];
    void ** slot = (seed >> 60) & 1 ? &holder->a : &holder->b;
    ASSERT_EQ(grheap_store(w.heap, holder, slot, value), GRHEAP_OK);
    if (step % 5000 == 4999) {
      ASSERT_EQ(violations_of(w), 0u) << step;
    }
  }
  EXPECT_EQ(violations_of(w), 0u);
  // Drop half and collect: the shadow of the dead is removed, the rest holds.
  for (int i = 0; i < kN; i += 2) {
    ASSERT_EQ(grheap_handle_destroy(hs[i]), GRHEAP_OK);
    hs[i] = nullptr;
  }
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().verify_violations, 0u);
  EXPECT_EQ(violations_of(w), 0u);
}

TEST_F(VerifyTest, AShadowThatCouldNotGrowIsReportedAndStoresStillWork) {
  HeapWorld w(verified());
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, a, &h), GRHEAP_OK);
  w.allocator.fail_all = true;
  Node * stored[80];
  w.allocator.fail_all = false;
  for (auto & s : stored) {
    s = new_node(w.heap, 7);
  }
  w.allocator.fail_all = true;
  for (auto * s : stored) {
    // The first insert needs a table; the allocator refuses it.
    ASSERT_EQ(grheap_store(w.heap, a, &a->a, s), GRHEAP_OK);
    EXPECT_EQ(a->a, s); // the program is unaffected
  }
  w.allocator.fail_all = false;
  uint64_t v = 1;
  EXPECT_EQ(grheap_verify(w.heap, &v), GRHEAP_ERR_OOM);
  EXPECT_EQ(v, 1u);
  // A collection does not abort or count false violations either.
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().verify_violations, 0u);
}

TEST_F(VerifyTest, RefusesNullAndNonOwnersAndACallFromInsideATrace) {
  HeapWorld w(verified());
  uint64_t v = 5;
  EXPECT_EQ(grheap_verify(nullptr, &v), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_verify(w.heap, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(v, 5u);
  std::thread other([&] {
    EXPECT_EQ(grheap_verify(w.heap, &v), GRHEAP_ERR_INVALID);
  });
  other.join();
  EXPECT_EQ(v, 5u);

  static GRHEAP_Heap * the_heap;
  static GRHEAP_Result inside;
  the_heap = w.heap;
  inside = GRHEAP_OK;
  static const GRHEAP_Type tracing = GRHEAP_TYPE_INIT("tracing", 16,
      [](GRHEAP_Tracer *, void *) {
        uint64_t x;
        inside = grheap_verify(the_heap, &x);
      },
      nullptr, nullptr, nullptr, nullptr);
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &tracing, &p), GRHEAP_OK);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, p, &h), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(inside, GRHEAP_ERR_INVALID);
}

TEST_F(VerifyTest, TheShadowMigratesWithTheContext) {
  HeapWorld w(verified());
  Node * a = new_node(w.heap, 1);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, a, &h), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, a, &a->a, a), GRHEAP_OK);
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  std::thread other([&] {
    ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    ASSERT_EQ(grheap_store(w.heap, a, &a->b, a), GRHEAP_OK);
    uint64_t v = 9;
    ASSERT_EQ(grheap_verify(w.heap, &v), GRHEAP_OK);
    EXPECT_EQ(v, 0u);
    ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  });
  other.join();
  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  EXPECT_EQ(violations_of(w), 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
