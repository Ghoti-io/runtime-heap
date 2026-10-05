/**
 * @file
 *
 * GC torture: a collection at every GC point the heap owns, and freed cells
 * poisoned, so that a root that is missing shows itself at once.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
/* MSYS2 has no setenv/unsetenv; the CRT's _putenv_s sets, and "NAME=" removes. */
static int setenv(const char * n, const char * v, int) {
  return _putenv_s(n, v);
}
static int unsetenv(const char * n) {
  return _putenv_s(n, "");
}
#endif

namespace {

/* Sets or unsets an environment variable for the life of the object. */
struct EnvGuard {
  std::string name;
  std::string old;
  bool had;
  EnvGuard(const char * n, const char * value) : name(n) {
    const char * o = std::getenv(n);
    had = o != nullptr;
    if (had) {
      old = o;
    }
    if (value != nullptr) {
      setenv(n, value, 1);
    } else {
      unsetenv(n);
    }
  }
  ~EnvGuard() {
    if (had) {
      setenv(name.c_str(), old.c_str(), 1);
    } else {
      unsetenv(name.c_str());
    }
  }
};

/* A payload of 64 bytes of a known pattern, and no pointers. */
struct Blob {
  unsigned char bytes[64];
};
const GRHEAP_Type kBlob = GRHEAP_TYPE_INIT("blob", sizeof(Blob), nullptr, nullptr, nullptr, nullptr, nullptr);

WorldConfig tortured() {
  WorldConfig c;
  c.torture = 1;
  c.verify = 0;
  return c;
}

} // namespace

class TortureTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

TEST_F(TortureTest, EveryAllocationCollectsFirst) {
  HeapWorld w(tortured());
  void * head = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &head), GRHEAP_OK);
  for (int i = 0; i < 50; i++) {
    Node * n = new_node(w.heap, static_cast<uint64_t>(i), &kPlainNode);
    ASSERT_EQ(grheap_store(w.heap, n, &n->a, head), GRHEAP_OK);
    head = n; // rooted before the next allocation
  }
  GRHEAP_Stats s = w.stats();
  EXPECT_EQ(s.allocations, 50u);
  EXPECT_EQ(s.torture_collections, 50u);
  EXPECT_EQ(s.collections, 50u);
  EXPECT_EQ(s.live_objects, 50u);
  ASSERT_EQ(grheap_root_remove(w.heap, &head), GRHEAP_OK);
}

TEST_F(TortureTest, ARefusedAllocationStillCollectedFirstAndAHardFailureDoesNotCollectTwice) {
  HeapWorld w(tortured());
  uint64_t before = w.stats().collections;
  void * p;
  w.allocator.fail_all = true;
  EXPECT_EQ(grheap_alloc(w.heap, &kLeaf, &p), GRHEAP_ERR_OOM);
  w.allocator.fail_all = false;
  // The torture collection ran; the retry-after-collection was not added to it.
  EXPECT_EQ(w.stats().collections - before, 1u);
  EXPECT_EQ(w.stats().torture_collections, 1u);
}

TEST_F(TortureTest, AnObjectHeldOnlyInACVariableIsFreedAndItsCellPoisoned) {
  HeapWorld w(tortured());
  // A survivor of the same size class keeps the block alive, so the freed
  // cell stays inside a mapped block (as ASan would never see).
  void * keeper_p;
  ASSERT_EQ(grheap_alloc(w.heap, &kBlob, &keeper_p), GRHEAP_OK);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, keeper_p, &h), GRHEAP_OK);
  void * victim_p;
  ASSERT_EQ(grheap_alloc(w.heap, &kBlob, &victim_p), GRHEAP_OK);
  auto * victim = static_cast<unsigned char *>(victim_p);
  memset(victim, 0x41, sizeof(Blob));
  // The next allocation is a GC point; the victim is in no root. The new
  // object is of another size class, so it does not take the victim's cell.
  void * other;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kBlob, 400, &other), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, victim));
  EXPECT_TRUE(destroyed_log().empty());
  // Every payload byte reads as poison: 0xDD, not the 0x41 that was written
  // and not a zero.
  for (size_t i = 0; i < sizeof(Blob); i++) {
    ASSERT_EQ(victim[i], 0xDD) << i;
  }
  ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
}

TEST_F(TortureTest, WithoutTortureTheSameProgramReadsItsOwnDataBack) {
  WorldConfig config = steady();
  HeapWorld w(config);
  void * keeper_p;
  ASSERT_EQ(grheap_alloc(w.heap, &kBlob, &keeper_p), GRHEAP_OK);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, keeper_p, &h), GRHEAP_OK);
  void * victim_p;
  ASSERT_EQ(grheap_alloc(w.heap, &kBlob, &victim_p), GRHEAP_OK);
  auto * victim = static_cast<unsigned char *>(victim_p);
  memset(victim, 0x41, sizeof(Blob));
  void * other;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kBlob, 400, &other), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, victim));
  EXPECT_EQ(w.stats().collections, 0u);
  EXPECT_EQ(victim[0], 0x41);
  // An explicit collection frees it, but without torture nothing is poisoned:
  // only the free-list link overwrote the cell's first word.
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, victim));
  EXPECT_EQ(victim[16], 0x41);
  EXPECT_NE(victim[16], 0xDD);
  ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
}

TEST_F(TortureTest, ACellTakenAgainIsZeroedNotPoisoned) {
  HeapWorld w(tortured());
  void * keeper_p;
  ASSERT_EQ(grheap_alloc(w.heap, &kBlob, &keeper_p), GRHEAP_OK);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, keeper_p, &h), GRHEAP_OK);
  void * a;
  ASSERT_EQ(grheap_alloc(w.heap, &kBlob, &a), GRHEAP_OK);
  void * b;
  ASSERT_EQ(grheap_alloc(w.heap, &kBlob, &b), GRHEAP_OK); // a is freed first
  EXPECT_EQ(b, a); // the one free cell of the class
  for (size_t i = 0; i < sizeof(Blob); i++) {
    ASSERT_EQ(static_cast<unsigned char *>(b)[i], 0) << i;
  }
  ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
}

TEST_F(TortureTest, EveryActPollCollects) {
  HeapWorld w(tortured());
  void * keep = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &keep), GRHEAP_OK);
  Node * a = new_node(w.heap, 1);
  keep = a;
  new_node(w.heap, 2); // garbage: nothing allocates after this line
  EXPECT_FALSE(destroyed(2)); // ... so the garbage is still there
  uint64_t before = w.stats().torture_collections;
  // A pending request is what takes a poll off its fast path, which runs
  // nothing; the interrupt makes the next poll run the phases once.
  GRCORE_Port * port;
  ASSERT_EQ(grcore_context_port(w.ctx, &port), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port, GRCORE_REQUEST_INTERRUPT), GRCORE_OK);
  grcore_port_release(port);
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
  EXPECT_EQ(outcome, GRCORE_OUTCOME_PAUSED);
  EXPECT_GE(w.stats().torture_collections - before, 1u);
  EXPECT_TRUE(destroyed(2)); // the poll's collection freed the garbage
  EXPECT_FALSE(destroyed(1));
  ASSERT_EQ(grcore_context_clear_request(w.ctx, GRCORE_REQUEST_INTERRUPT),
      GRCORE_OK);
  ASSERT_EQ(grcore_resume(w.ctx, &outcome), GRCORE_OK);
  EXPECT_EQ(outcome, GRCORE_OUTCOME_FINISHED);
  ASSERT_EQ(grheap_root_remove(w.heap, &keep), GRHEAP_OK);
}

TEST_F(TortureTest, APollThatAlsoAsksToReclaimCollectsOnce) {
  WorldConfig config = tortured();
  config.memory = 100000;
  config.reserve = 400000;
  HeapWorld w(config);
  // Live objects past the budget (garbage would be collected away by torture
  // as it was made), so the poll has something to ask about.
  void * p;
  while (grcore_context_memory_in_use(w.ctx) <= 100000) {
    // Rooted, so torture keeps it and the heap really fills.
    ASSERT_EQ(grheap_alloc(w.heap, &kBlob, &p), GRHEAP_OK);
    GRHEAP_Handle * h; // freed with the context
    ASSERT_EQ(grheap_handle_create(w.heap, p, &h), GRHEAP_OK);
  }
  uint64_t before = w.stats().collections;
  GRCORE_Outcome outcome;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_POLL(c);
    return GRCORE_STEP_FINISHED;
  }};
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(w.stats().collections - before, 1u);
}

TEST_F(TortureTest, AnArenaIgnoresTorture) {
  WorldConfig config = tortured();
  config.mode = GRHEAP_MODE_ARENA;
  HeapWorld w(config);
  for (int i = 0; i < 100; i++) {
    new_node(w.heap, static_cast<uint64_t>(i));
  }
  GRCORE_Outcome outcome;
  Fn fn{[&](GRCORE_Context * c) {
    GRCORE_POLL(c);
    return GRCORE_STEP_FINISHED;
  }};
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  GRHEAP_Stats s = w.stats();
  EXPECT_EQ(s.torture_collections, 0u);
  EXPECT_EQ(s.collections, 0u);
  EXPECT_EQ(s.live_objects, 100u);
  EXPECT_TRUE(destroyed_log().empty());
}

TEST_F(TortureTest, ARootedProgramRunsTheSameUnderTortureAsWithout) {
  // A small deterministic build-and-drop program; what is left must not depend
  // on how often the collector ran.
  auto run = [](bool torture) {
    WorldConfig config;
    config.torture = torture ? 1 : 0;
    config.verify = torture ? GRHEAP_VERIFY_COUNT : GRHEAP_VERIFY_OFF;
    HeapWorld w(config);
    destroyed_log().clear();
    void * roots[4] = {};
    for (auto & r : roots) {
      EXPECT_EQ(grheap_root_add(w.heap, &r), GRHEAP_OK);
    }
    uint64_t seed = 12345;
    for (int i = 0; i < 400; i++) {
      seed = seed * 6364136223846793005ull + 1442695040888963407ull;
      Node * n = new_node(w.heap, static_cast<uint64_t>(i));
      void ** slot = &roots[(seed >> 33) % 4];
      EXPECT_EQ(grheap_store(w.heap, n, &n->a, *slot), GRHEAP_OK);
      *slot = n; // each root slot is a growing list
      if ((seed >> 40) % 5 == 0) {
        *slot = nullptr; // and sometimes dropped whole
      }
    }
    EXPECT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    std::vector<uint64_t> alive;
    for (auto * r : roots) {
      for (auto * n = static_cast<Node *>(r); n != nullptr;
          n = static_cast<Node *>(n->a)) {
        alive.push_back(n->id);
      }
    }
    for (auto & r : roots) {
      EXPECT_EQ(grheap_root_remove(w.heap, &r), GRHEAP_OK);
    }
    EXPECT_EQ(w.stats().verify_violations, 0u);
    return alive;
  };
  EXPECT_EQ(run(false), run(true));
}

/* ---- The defaults, from the environment ---------------------------------- */

TEST_F(TortureTest, TheEnvironmentSetsTheDefaultOfANewOptionsObjectAndASetterWins) {
  GRHEAP_Options * o;
  {
    EnvGuard t("GRHEAP_TORTURE", nullptr);
    EnvGuard v("GRHEAP_VERIFY", nullptr);
    ASSERT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK);
    EXPECT_FALSE(grheap_options_get_torture(o));
    EXPECT_EQ(grheap_options_get_verify(o), GRHEAP_VERIFY_OFF);
    grheap_options_destroy(o);
  }
  {
    EnvGuard t("GRHEAP_TORTURE", "1");
    EnvGuard v("GRHEAP_VERIFY", "1");
    ASSERT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK);
    EXPECT_TRUE(grheap_options_get_torture(o));
    EXPECT_EQ(grheap_options_get_verify(o), GRHEAP_VERIFY_ABORT);
    // An explicit setter wins.
    ASSERT_EQ(grheap_options_set_torture(o, false), GRHEAP_OK);
    ASSERT_EQ(grheap_options_set_verify(o, GRHEAP_VERIFY_OFF), GRHEAP_OK);
    EXPECT_FALSE(grheap_options_get_torture(o));
    EXPECT_EQ(grheap_options_get_verify(o), GRHEAP_VERIFY_OFF);
    grheap_options_destroy(o);
  }
  {
    EnvGuard t("GRHEAP_TORTURE", "0");
    EnvGuard v("GRHEAP_VERIFY", "count");
    ASSERT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK);
    EXPECT_FALSE(grheap_options_get_torture(o));
    EXPECT_EQ(grheap_options_get_verify(o), GRHEAP_VERIFY_COUNT);
    grheap_options_destroy(o);
  }
  {
    EnvGuard v("GRHEAP_VERIFY", "banana");
    ASSERT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK);
    EXPECT_EQ(grheap_options_get_verify(o), GRHEAP_VERIFY_OFF);
    grheap_options_destroy(o);
  }
}

TEST_F(TortureTest, SettersAndGettersRefuseWhatTheyShould) {
  EXPECT_EQ(grheap_options_set_torture(nullptr, true), GRHEAP_ERR_INVALID);
  EXPECT_FALSE(grheap_options_get_torture(nullptr));
  EXPECT_EQ(grheap_options_set_verify(nullptr, GRHEAP_VERIFY_COUNT),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_get_verify(nullptr), GRHEAP_VERIFY_OFF);
  GRHEAP_Options * o;
  ASSERT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK);
  GRHEAP_Verify before = grheap_options_get_verify(o);
  EXPECT_EQ(grheap_options_set_verify(o, static_cast<GRHEAP_Verify>(9)),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_get_verify(o), before);
  grheap_options_destroy(o);
}

TEST_F(TortureTest, AHeapMadeWithNoOptionsTakesTheEnvironmentToo) {
  EnvGuard t("GRHEAP_TORTURE", "1");
  EnvGuard v("GRHEAP_VERIFY", "count");
  WorldConfig config;
  config.create_heap = false;
  HeapWorld w(config);
  GRHEAP_Heap * heap = nullptr;
  ASSERT_EQ(grheap_heap_create(w.ctx, nullptr, &heap), GRHEAP_OK);
  void * p;
  ASSERT_EQ(grheap_alloc(heap, &kLeaf, &p), GRHEAP_OK);
  GRHEAP_Stats s;
  ASSERT_EQ(grheap_stats(heap, &s), GRHEAP_OK);
  EXPECT_EQ(s.torture_collections, 1u);
  uint64_t violations = 1;
  ASSERT_EQ(grheap_verify(heap, &violations), GRHEAP_OK);
  EXPECT_EQ(violations, 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
