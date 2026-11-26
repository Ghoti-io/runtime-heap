/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Runtime-heap.
 *
 * Ghoti.io Runtime-heap is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Runtime-heap is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Mark-step torture: a store that is right under a stop-the-world collector and
 * wrong when marking interleaves is found. The defects are planted in the
 * test's own mutator code, each with the same mutation written correctly as its
 * control, and a random mutator that only uses the heap's store entries shows
 * the instrument has no false positive.
 */

#include "../test_helpers.h"

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <gtest/gtest.h>

#include <random>

namespace {

WorldConfig torture_config(GRHEAP_MarkTorture mode = GRHEAP_MARK_TORTURE_COUNT) {
  WorldConfig c = steady();
  c.mark_torture = mode;
  c.verify = 0;  // the planted stores are raw on purpose
  return c;
}

void point(HeapWorld & w) {
  ASSERT_EQ(grheap_gc_point(w.heap, GRHEAP_GC_POINT_CALL_OUT), GRHEAP_OK);
}

// Runs GC points until a cycle has just finished, leaving the next one to use
// the insertion barrier (`deletion` false) or the deletion barrier (true):
// cycles alternate, starting with insertion.
void to_boundary(HeapWorld & w, bool deletion) {
  for (int guard = 0; guard < 10000; guard++) {
    uint64_t before = w.stats().mark_cycles;
    point(w);
    uint64_t now = w.stats().mark_cycles;
    if (now != before && (now % 2 == 1) == deletion) {
      return;
    }
  }
  FAIL() << "no cycle boundary was reached";
}

void finish_cycle(HeapWorld & w) {
  uint64_t before = w.stats().mark_cycles;
  for (int guard = 0; guard < 10000 && w.stats().mark_cycles == before; guard++) {
    point(w);
  }
  ASSERT_GT(w.stats().mark_cycles, before);
}

// A <- root, A -> M -> B, and a root slot for a new object N.
struct Graph {
  void * a = nullptr;
  void * n = nullptr;
  Node * A() { return static_cast<Node *>(a); }
  Node * M() { return static_cast<Node *>(A()->a); }
  Node * N() { return static_cast<Node *>(n); }
};

void build(HeapWorld & w, Graph & g) {
  ASSERT_EQ(grheap_root_add(w.heap, &g.a), GRHEAP_OK);
  ASSERT_EQ(grheap_root_add(w.heap, &g.n), GRHEAP_OK);
  ASSERT_EQ(grheap_alloc(w.heap, &kPlainNode, &g.a), GRHEAP_OK);
  Node * m = new_node(w.heap, 2, &kPlainNode);
  ASSERT_EQ(grheap_store(w.heap, g.A(), &g.A()->a, m), GRHEAP_OK);
  Node * b = new_node(w.heap, 3, &kPlainNode);
  ASSERT_EQ(grheap_store(w.heap, g.M(), &g.M()->a, b), GRHEAP_OK);
}

enum class Plant { None, InitStore, RawRemoval };

// Moves B from M into a new object N while a cycle is running, correctly or
// with one store made without the heap's entry.
void move_b(HeapWorld & w, Graph & g, Plant plant, bool deletion_cycle) {
  to_boundary(w, deletion_cycle);
  point(w);  // the cycle starts: A is grey
  // The allocation's own point traces A, which shades M; N is born marked.
  ASSERT_EQ(grheap_alloc(w.heap, &kPlainNode, &g.n), GRHEAP_OK);
  void * b = g.M()->a;
  if (plant == Plant::InitStore) {
    g.N()->a = b;  // an initializing store without the barrier
  } else {
    ASSERT_EQ(grheap_store(w.heap, g.N(), &g.N()->a, b), GRHEAP_OK);
  }
  if (plant == Plant::RawRemoval) {
    g.M()->a = nullptr;  // the reference leaves M without a store
  } else {
    ASSERT_EQ(grheap_store(w.heap, g.M(), &g.M()->a, nullptr), GRHEAP_OK);
  }
  finish_cycle(w);
}

uint64_t violations_for(Plant plant, bool deletion_cycle) {
  HeapWorld w(torture_config());
  Graph g;
  build(w, g);
  move_b(w, g, plant, deletion_cycle);
  return w.stats().mark_violations;
}

}  // namespace

TEST(MarkTorture, AnInitializingStoreWithoutTheBarrierIsCaughtWhenMarkingInterleaves) {
  EXPECT_GT(violations_for(Plant::InitStore, false), 0u);
  // Control: the same mutation through the store entry.
  EXPECT_EQ(violations_for(Plant::None, false), 0u);
}

TEST(MarkTorture, ARemovalWithoutAStoreIsCaughtUnderTheDeletionDiscipline) {
  EXPECT_GT(violations_for(Plant::RawRemoval, true), 0u);
  EXPECT_EQ(violations_for(Plant::None, true), 0u);
}

TEST(MarkTorture, TheInitializingStoreIsRightUnderAStopTheWorldCollector) {
  // The same defect with the instrument off: nothing is lost, which is why
  // only an interleaved mark finds it.
  WorldConfig c = steady();
  c.verify = 0;  // a raw store on purpose
  HeapWorld w(c);
  Graph g;
  build(w, g);
  ASSERT_EQ(grheap_alloc(w.heap, &kPlainNode, &g.n), GRHEAP_OK);
  void * b = g.M()->a;
  g.N()->a = b;
  ASSERT_EQ(grheap_store(w.heap, g.M(), &g.M()->a, nullptr), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, b));
  EXPECT_EQ(w.stats().mark_steps, 0u);
}

TEST(MarkTorture, AbortModeNamesTheObjectAndStops) {
  EXPECT_DEATH(
      {
        HeapWorld w(torture_config(GRHEAP_MARK_TORTURE_ABORT));
        Graph g;
        build(w, g);
        move_b(w, g, Plant::InitStore, false);
      },
      "mark-torture: a reachable object was not marked");
}

namespace {

struct Eph {
  void * key;
  void * value;
};
const GRHEAP_WeakSpec kEphSpec = {GRHEAP_EPHEMERON, 0, offsetof(Eph, key), offsetof(Eph, value), false, nullptr};
const GRHEAP_Type kEph = GRHEAP_TYPE_INIT("eph", sizeof(Eph), nullptr, nullptr, &kEphSpec, nullptr, nullptr);

// An encoded cell: slot a in the type's low-tag codec, slot b in another.
const GRHEAP_ValueCodec kLow = {7, 1, ~UINT64_C(7), 0, 0};
const GRHEAP_ValueCodec kShifted = {0xF, 0x5, ~UINT64_C(0xF), 4, 0};
struct Two {
  uint64_t a;
  uint64_t b;
};
void two_trace(GRHEAP_Tracer * t, void * p) {
  auto * c = static_cast<Two *>(p);
  grheap_trace_word(t, &c->a);
  grheap_trace_word_with(t, &c->b, &kShifted);
}
const GRHEAP_Type kTwo = GRHEAP_TYPE_INIT_CODEC("two", sizeof(Two), two_trace, nullptr,
    nullptr, nullptr, nullptr, &kLow);

}  // namespace

TEST(MarkTorture, ARandomMutatorThatUsesTheStoreEntriesHasNoViolation) {
  for (unsigned seed = 1; seed <= 8; seed++) {
    HeapWorld w(torture_config());
    std::mt19937 rng(seed);
    constexpr int kRoots = 12;
    std::vector<void *> roots(kRoots, nullptr);
    std::vector<int> kinds(kRoots, 0);  // 0 node, 1 eph, 2 two
    for (void *& r : roots) {
      ASSERT_EQ(grheap_root_add(w.heap, &r), GRHEAP_OK);
    }
    for (int step = 0; step < 6000; step++) {
      int i = static_cast<int>(rng() % kRoots);
      int j = static_cast<int>(rng() % kRoots);
      switch (rng() % 10) {
        case 0:
          kinds[i] = static_cast<int>(rng() % 3);
          ASSERT_EQ(grheap_alloc(w.heap,
                        kinds[i] == 0 ? &kPlainNode : kinds[i] == 1 ? &kEph : &kTwo, &roots[i]),
              GRHEAP_OK);
          break;
        case 1:
          roots[i] = nullptr;
          break;
        case 2:
        case 3:  // store a reference (or nothing) into a heap object
          if (roots[i] != nullptr) {
            void * v = (rng() % 4) != 0 ? roots[j] : nullptr;
            if (kinds[i] == 0) {
              auto * n = static_cast<Node *>(roots[i]);
              ASSERT_EQ(grheap_store(w.heap, n, (rng() & 1) ? &n->a : &n->b, v), GRHEAP_OK);
            } else if (kinds[i] == 1) {
              auto * e = static_cast<Eph *>(roots[i]);
              // The key first: a value with a NULL key is cleared.
              ASSERT_EQ(grheap_store(w.heap, e, &e->key, v), GRHEAP_OK);
              ASSERT_EQ(grheap_store(w.heap, e, &e->value, roots[(j + 1) % kRoots]), GRHEAP_OK);
            } else {
              auto * c = static_cast<Two *>(roots[i]);
              uint64_t word = v == nullptr ? 0 : addr(v) | 1u;
              ASSERT_EQ(grheap_store_word(w.heap, c, &c->a, word), GRHEAP_OK);
              ASSERT_EQ(grheap_store_word(w.heap, c, &c->b,
                            v == nullptr ? 0 : (addr(v) << 4) | 5u),
                  GRHEAP_OK);
            }
          }
          break;
        case 4:  // copy a reference out of a node into a root
          if (roots[i] != nullptr && kinds[i] == 0) {
            roots[j] = static_cast<Node *>(roots[i])->a;
            kinds[j] = 0;
            if (roots[j] != nullptr) {
              const GRHEAP_Type * t = grheap_type_of(w.heap, roots[j]);
              kinds[j] = t == &kEph ? 1 : t == &kTwo ? 2 : 0;
            }
          }
          break;
        case 5:  // hang a chain off a node, so references sit deep in the graph
          if (roots[i] != nullptr && kinds[i] == 0) {
            Node * deep = nullptr;
            ASSERT_EQ(grheap_alloc(w.heap, &kPlainNode, reinterpret_cast<void **>(&roots[j])), GRHEAP_OK);
            kinds[j] = 0;
            if (roots[i] == nullptr || kinds[i] != 0) {
              break;
            }
            auto * top = static_cast<Node *>(roots[i]);
            deep = static_cast<Node *>(roots[j]);
            ASSERT_EQ(grheap_store(w.heap, deep, &deep->a, top->a), GRHEAP_OK);
            ASSERT_EQ(grheap_store(w.heap, top, &top->a, deep), GRHEAP_OK);
            roots[j] = nullptr;
          }
          break;
        case 6:  // move a deep reference into another node, then cut the old edge
          if (roots[i] != nullptr && kinds[i] == 0 && roots[j] != nullptr && kinds[j] == 0) {
            auto * from = static_cast<Node *>(roots[i]);
            auto * to = static_cast<Node *>(roots[j]);
            auto * mid = static_cast<Node *>(from->a);
            if (mid != nullptr && grheap_type_of(w.heap, mid) == &kPlainNode && mid != to) {
              ASSERT_EQ(grheap_store(w.heap, to, &to->b, mid->a), GRHEAP_OK);
              ASSERT_EQ(grheap_store(w.heap, mid, &mid->a, nullptr), GRHEAP_OK);
            }
          }
          break;
        default:
          point(w);
          break;
      }
    }
    GRHEAP_Stats s = w.stats();
    EXPECT_EQ(s.mark_violations, 0u) << "seed " << seed;
    EXPECT_GT(s.mark_cycles, 10u) << "seed " << seed << ": both disciplines ran";
    EXPECT_EQ(s.invalid_pointers, 0u) << "seed " << seed;
  }
}

TEST(MarkTorture, ItSuppressesTheCollectAtEveryPointAndThreshold) {
  WorldConfig c = torture_config();
  c.torture = 1;
  c.threshold = 1;
  HeapWorld w(c);
  void * root = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  ASSERT_EQ(grheap_alloc(w.heap, &kPlainNode, &root), GRHEAP_OK);
  point(w);
  EXPECT_EQ(w.stats().torture_collections, 0u);
  EXPECT_GT(w.stats().mark_steps, 0u);
  uint64_t seen = 0, acted = 0;
  ASSERT_EQ(grheap_gc_point_counts(w.heap, GRHEAP_GC_POINT_CALL_OUT, &seen, &acted), GRHEAP_OK);
  EXPECT_EQ(acted, 1u) << "a point the mode stepped at counts as acted on";
}

TEST(MarkTorture, AnExplicitCollectionAbandonsTheCycleWithoutAViolation) {
  HeapWorld w(torture_config());
  Graph g;
  build(w, g);
  to_boundary(w, false);
  point(w);
  point(w);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().mark_violations, 0u);
  finish_cycle(w);
  EXPECT_EQ(w.stats().mark_violations, 0u);
  EXPECT_EQ(g.M()->id, 2u);
}

TEST(MarkTorture, TheOptionIsCheckedAndDefaultsOff) {
  GRHEAP_Options * o;
  ASSERT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK);
  EXPECT_EQ(grheap_options_get_mark_torture(o), GRHEAP_MARK_TORTURE_OFF);
  EXPECT_EQ(grheap_options_set_mark_torture(o, static_cast<GRHEAP_MarkTorture>(3)), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_set_mark_torture(nullptr, GRHEAP_MARK_TORTURE_COUNT), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_set_mark_torture(o, GRHEAP_MARK_TORTURE_COUNT), GRHEAP_OK);
  EXPECT_EQ(grheap_options_get_mark_torture(o), GRHEAP_MARK_TORTURE_COUNT);
  grheap_options_destroy(o);
  EXPECT_EQ(grheap_options_get_mark_torture(nullptr), GRHEAP_MARK_TORTURE_OFF);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
