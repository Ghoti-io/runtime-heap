/**
 * @file
 *
 * A randomised oracle for the collector (story 6's deferred finding): a
 * deterministic generator builds graphs out of roots, handles, pins, edges
 * written through `grheap_store` and weak cells, mutates them and collects,
 * and after each explicit collection compares what survives against the
 * reachability set the test computes for itself.
 *
 * The test only ever touches objects that are reachable in its own model, so
 * it holds up under torture (a collection at every allocation) and with
 * barrier-verify on, which is how it runs below.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstddef>
#include <string>

namespace {

/* A Node's layout and a weak slot after it. */
struct WNode {
  void * a;
  void * b;
  uint64_t id;
  void * target;
};

uint64_t g_cleared = 0;

void wnode_cleared(void *) { g_cleared++; }

const GRHEAP_WeakSpec kWNodeSpec = {GRHEAP_WEAK_CELL, offsetof(WNode, target), 0,
    0, false, wnode_cleared};
const GRHEAP_Type kWNode = GRHEAP_TYPE_INIT("wnode", sizeof(WNode), node_trace, node_destroy,
    &kWNodeSpec, nullptr, nullptr);

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint64_t next() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
  }
  uint64_t below(uint64_t n) { return (next() >> 11) % n; }
};

constexpr int kRoots = 6;

struct Oracle {
  struct M {
    int a = -1;
    int b = -1;
    int wt = -1;
    bool weak = false;
    int pins = 0;
    GRHEAP_Handle * handle = nullptr;
  };

  HeapWorld & w;
  Rng rng;
  std::vector<M> m;
  std::vector<void *> obj;
  void * root_slot[kRoots] = {};
  int root_of[kRoots];

  Oracle(HeapWorld & world, uint64_t seed) : w(world), rng(seed) {
    for (int i = 0; i < kRoots; i++) {
      root_of[i] = -1;
      EXPECT_EQ(grheap_root_add(w.heap, &root_slot[i]), GRHEAP_OK);
    }
  }
  ~Oracle() {
    for (int i = 0; i < kRoots; i++) {
      EXPECT_EQ(grheap_root_remove(w.heap, &root_slot[i]), GRHEAP_OK);
    }
  }

  std::vector<bool> reach() const {
    std::vector<bool> seen(m.size(), false);
    std::vector<int> stack;
    auto push = [&](int i) {
      if (i >= 0 && !seen[static_cast<size_t>(i)]) {
        seen[static_cast<size_t>(i)] = true;
        stack.push_back(i);
      }
    };
    for (int r : root_of) {
      push(r);
    }
    for (size_t i = 0; i < m.size(); i++) {
      if (m[i].handle != nullptr || m[i].pins > 0) {
        push(static_cast<int>(i));
      }
    }
    while (!stack.empty()) {
      int i = stack.back();
      stack.pop_back();
      push(m[static_cast<size_t>(i)].a);
      push(m[static_cast<size_t>(i)].b);
    }
    return seen;
  }

  static std::vector<int> members(const std::vector<bool> & r) {
    std::vector<int> v;
    for (size_t i = 0; i < r.size(); i++) {
      if (r[i]) {
        v.push_back(static_cast<int>(i));
      }
    }
    return v;
  }

  void ** slot_a(int i) { return &static_cast<WNode *>(obj[static_cast<size_t>(i)])->a; }
  void ** slot_b(int i) { return &static_cast<WNode *>(obj[static_cast<size_t>(i)])->b; }

  void op_alloc() {
    bool weak = rng.below(10) < 3;
    void * p = nullptr;
    ASSERT_EQ(grheap_alloc(w.heap, weak ? &kWNode : &kNode, &p), GRHEAP_OK);
    int idx = static_cast<int>(m.size());
    static_cast<WNode *>(p)->id = static_cast<uint64_t>(idx);
    M entry;
    entry.weak = weak;
    // Rooted before anything else can allocate.
    ASSERT_EQ(grheap_handle_create(w.heap, p, &entry.handle), GRHEAP_OK);
    m.push_back(entry);
    obj.push_back(p);
  }

  void step() {
    std::vector<bool> r = reach();
    std::vector<int> live = members(r);
    uint64_t pick = rng.below(100);
    if (live.empty() || pick < 22) {
      op_alloc();
      return;
    }
    auto any = [&]() { return live[rng.below(live.size())]; };
    if (pick < 52) { // an edge
      int h = any();
      int t = rng.below(5) == 0 ? -1 : any();
      bool use_a = rng.below(2) == 0;
      void ** slot = use_a ? slot_a(h) : slot_b(h);
      ASSERT_EQ(grheap_store(w.heap, obj[static_cast<size_t>(h)], slot,
                    t < 0 ? nullptr : obj[static_cast<size_t>(t)]),
          GRHEAP_OK);
      (use_a ? m[static_cast<size_t>(h)].a : m[static_cast<size_t>(h)].b) = t;
    } else if (pick < 62) { // a weak target
      std::vector<int> weaks;
      for (int i : live) {
        if (m[static_cast<size_t>(i)].weak) {
          weaks.push_back(i);
        }
      }
      if (weaks.empty()) {
        return;
      }
      int c = weaks[rng.below(weaks.size())];
      int t = rng.below(6) == 0 ? -1 : any();
      auto * cell = static_cast<WNode *>(obj[static_cast<size_t>(c)]);
      ASSERT_EQ(grheap_store(w.heap, cell, &cell->target,
                    t < 0 ? nullptr : obj[static_cast<size_t>(t)]),
          GRHEAP_OK);
      m[static_cast<size_t>(c)].wt = t;
    } else if (pick < 72) { // a root slot, with a NULL holder
      int k = static_cast<int>(rng.below(kRoots));
      int t = rng.below(4) == 0 ? -1 : any();
      ASSERT_EQ(grheap_store(w.heap, nullptr, &root_slot[k],
                    t < 0 ? nullptr : obj[static_cast<size_t>(t)]),
          GRHEAP_OK);
      root_of[k] = t;
    } else if (pick < 80) { // drop a handle
      std::vector<int> held;
      for (int i : live) {
        if (m[static_cast<size_t>(i)].handle != nullptr) {
          held.push_back(i);
        }
      }
      if (held.empty()) {
        return;
      }
      int i = held[rng.below(held.size())];
      ASSERT_EQ(grheap_handle_destroy(m[static_cast<size_t>(i)].handle),
          GRHEAP_OK);
      m[static_cast<size_t>(i)].handle = nullptr;
    } else if (pick < 85) { // add a handle
      int i = any();
      if (m[static_cast<size_t>(i)].handle == nullptr) {
        ASSERT_EQ(grheap_handle_create(w.heap, obj[static_cast<size_t>(i)],
                      &m[static_cast<size_t>(i)].handle),
            GRHEAP_OK);
      }
    } else if (pick < 90) { // pin
      int i = any();
      if (m[static_cast<size_t>(i)].pins < 4) {
        ASSERT_EQ(grheap_pin(w.heap, obj[static_cast<size_t>(i)]), GRHEAP_OK);
        m[static_cast<size_t>(i)].pins++;
      }
    } else if (pick < 94) { // unpin
      std::vector<int> pinned;
      for (int i : live) {
        if (m[static_cast<size_t>(i)].pins > 0) {
          pinned.push_back(i);
        }
      }
      if (pinned.empty()) {
        return;
      }
      int i = pinned[rng.below(pinned.size())];
      ASSERT_EQ(grheap_unpin(w.heap, obj[static_cast<size_t>(i)]), GRHEAP_OK);
      m[static_cast<size_t>(i)].pins--;
    } else { // collect, and compare with the model
      check_after_collect();
    }
  }

  void check_after_collect() {
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    std::vector<bool> r = reach();
    for (size_t i = 0; i < m.size(); i++) {
      if (r[i]) {
        ASSERT_EQ(destroyed_count(i), 0u) << "object " << i << " was freed";
        ASSERT_TRUE(grheap_contains(w.heap, obj[i])) << "object " << i;
        auto * n = static_cast<WNode *>(obj[i]);
        ASSERT_EQ(n->id, i);
        void * ea = m[i].a < 0 ? nullptr : obj[static_cast<size_t>(m[i].a)];
        void * eb = m[i].b < 0 ? nullptr : obj[static_cast<size_t>(m[i].b)];
        ASSERT_EQ(n->a, ea) << "object " << i << " slot a";
        ASSERT_EQ(n->b, eb) << "object " << i << " slot b";
        if (m[i].weak) {
          bool kept = m[i].wt >= 0 && r[static_cast<size_t>(m[i].wt)];
          void * et = kept ? obj[static_cast<size_t>(m[i].wt)] : nullptr;
          ASSERT_EQ(n->target, et) << "weak cell " << i;
        }
      } else {
        ASSERT_EQ(destroyed_count(i), 1u) << "object " << i << " not freed once";
      }
    }
    GRHEAP_Stats s = w.stats();
    uint64_t alive = 0;
    for (bool b : r) {
      alive += b ? 1u : 0u;
    }
    ASSERT_EQ(s.live_objects, alive);
    ASSERT_EQ(s.verify_violations, 0u);
    ASSERT_EQ(s.invalid_pointers, 0u);
    ASSERT_EQ(s.weak_cleared, g_cleared);
  }
};

void run_oracle(const WorldConfig & config, uint64_t seed, int ops) {
  HeapWorld w(unmoved(config));
  destroyed_log().clear();
  g_cleared = 0;
  {
    Oracle o(w, seed);
    for (int i = 0; i < ops && !::testing::Test::HasFatalFailure(); i++) {
      o.step();
    }
    if (!::testing::Test::HasFatalFailure()) {
      o.check_after_collect();
    }
    // Let go of everything: nothing is left.
    if (!::testing::Test::HasFatalFailure()) {
      for (auto & e : o.m) {
        if (e.handle != nullptr) {
          ASSERT_EQ(grheap_handle_destroy(e.handle), GRHEAP_OK);
          e.handle = nullptr;
        }
      }
      for (size_t i = 0; i < o.m.size(); i++) {
        while (o.m[i].pins > 0) {
          ASSERT_EQ(grheap_unpin(w.heap, o.obj[i]), GRHEAP_OK);
          o.m[i].pins--;
        }
      }
      for (int k = 0; k < kRoots; k++) {
        o.root_slot[k] = nullptr;
        o.root_of[k] = -1;
      }
      ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
      EXPECT_EQ(w.stats().live_objects, 0u);
      for (size_t i = 0; i < o.m.size(); i++) {
        EXPECT_EQ(destroyed_count(i), 1u) << i;
      }
    }
  }
}

} // namespace

class OracleTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

TEST_F(OracleTest, SurvivorsMatchReachabilityUnderTortureAndVerify) {
  for (uint64_t seed = 1; seed <= 6; seed++) {
    for (int ops : {250, 1200}) {
      SCOPED_TRACE("seed " + std::to_string(seed) + " ops " + std::to_string(ops));
      WorldConfig config;
      config.torture = 1;
      config.verify = GRHEAP_VERIFY_COUNT;
      run_oracle(config, seed, ops);
      if (HasFatalFailure()) {
        return;
      }
    }
  }
}

TEST_F(OracleTest, SurvivorsMatchReachabilityWithTheInstrumentsOff) {
  for (uint64_t seed = 11; seed <= 16; seed++) {
    for (int ops : {250, 4000}) {
      SCOPED_TRACE("seed " + std::to_string(seed) + " ops " + std::to_string(ops));
      WorldConfig config;
      config.torture = 0;
      config.verify = GRHEAP_VERIFY_OFF;
      run_oracle(config, seed, ops);
      if (HasFatalFailure()) {
        return;
      }
    }
  }
}

TEST_F(OracleTest, SurvivorsMatchReachabilityWithAutomaticCollectionsAtASmallThreshold) {
  for (uint64_t seed = 21; seed <= 26; seed++) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    WorldConfig config;
    config.torture = 0;
    config.verify = GRHEAP_VERIFY_COUNT;
    config.threshold = 1500; // an implicit collection every thirty objects
    run_oracle(config, seed, 3000);
    if (HasFatalFailure()) {
      return;
    }
  }
}

TEST_F(OracleTest, TheSameSeedGivesTheSameSurvivorsWhateverTheInstruments) {
  // Two runs of one program, one tortured and verified and one not, free the
  // same set of objects in the end (the order of destroy hooks may differ).
  auto freed = [](bool instruments) {
    WorldConfig config;
    config.torture = instruments ? 1 : 0;
    config.verify = instruments ? GRHEAP_VERIFY_COUNT : GRHEAP_VERIFY_OFF;
    HeapWorld w(unmoved(config));
    destroyed_log().clear();
    g_cleared = 0;
    std::vector<uint64_t> alive;
    {
      Oracle o(w, 77);
      for (int i = 0; i < 800; i++) {
        o.step();
      }
      o.check_after_collect();
      std::vector<bool> r = o.reach();
      for (size_t i = 0; i < r.size(); i++) {
        if (r[i]) {
          alive.push_back(i);
        }
      }
    }
    return alive;
  };
  // The generator picks from the model's reachable set, which is the same set
  // in both runs, so the programs are the same program.
  EXPECT_EQ(freed(false), freed(true));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
