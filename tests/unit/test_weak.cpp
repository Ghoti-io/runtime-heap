/**
 * @file
 *
 * Weak cells and ephemerons: what a collection clears, what it keeps, when the
 * `cleared` callback runs, and that none of it depends on when the collection
 * happens (the same tests run under torture, where it happens at every
 * allocation).
 *
 * Every object a test needs across an allocation is kept by a root slot, a
 * handle or a pin: that is the discipline torture polices.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstddef>
#include <cstring>
#include <thread>

namespace {

/* A weak cell: a strong slot `a`, an id, and the weak slot `target`. */
struct WeakCell {
  void * a;
  void * b;
  uint64_t id;
  void * target;
};

/* An ephemeron: id, then the key and the value. */
struct Eph {
  uint64_t id;
  void * key;
  void * value;
};

/* A weak cell whose slot holds a codec-encoded word. */
struct WordCell {
  uint64_t id;
  uint64_t target;
};

struct Event {
  void * payload;
  uint64_t id;
  void * slot_then;       // the weak slot as `cleared` saw it
  GRHEAP_Result alloc;    // what an allocation attempted from `cleared` got
  GRHEAP_Result collect;  // and a collection
  bool live_then;         // grheap_contains(payload) from inside
  size_t destroyed_then;  // how many destroy hooks had run
};

std::vector<Event> & events() {
  static std::vector<Event> e;
  return e;
}

GRHEAP_Heap * the_heap = nullptr;

void record(void * payload, uint64_t id, void * slot) {
  Event e;
  e.payload = payload;
  e.id = id;
  e.slot_then = slot;
  void * q;
  e.alloc = grheap_alloc(the_heap, &kLeaf, &q);
  e.collect = grheap_collect(the_heap);
  e.live_then = grheap_contains(the_heap, payload);
  e.destroyed_then = destroyed_log().size();
  events().push_back(e);
}

void weak_cleared(void * payload) {
  auto * c = static_cast<WeakCell *>(payload);
  record(payload, c->id, c->target);
}

void eph_cleared(void * payload) {
  auto * e = static_cast<Eph *>(payload);
  record(payload, e->id, e->key != nullptr ? e->key : e->value);
}

void word_cleared(void * payload) {
  auto * c = static_cast<WordCell *>(payload);
  record(payload, c->id, reinterpret_cast<void *>(c->target));
}

void weak_trace(GRHEAP_Tracer * t, void * payload) {
  auto * c = static_cast<WeakCell *>(payload);
  grheap_trace_slot(t, &c->a);
  grheap_trace_slot(t, &c->b);
}

void cell_destroy(void * payload) {
  destroyed_log().push_back(static_cast<WeakCell *>(payload)->id);
}

const GRHEAP_WeakSpec kCellSpec = {GRHEAP_WEAK_CELL, offsetof(WeakCell, target),
    0, 0, false, weak_cleared};
const GRHEAP_WeakSpec kEphSpec = {GRHEAP_EPHEMERON, 0, offsetof(Eph, key),
    offsetof(Eph, value), false, eph_cleared};
const GRHEAP_WeakSpec kWordSpec = {GRHEAP_WEAK_CELL, offsetof(WordCell, target),
    0, 0, true, word_cleared};
const GRHEAP_WeakSpec kWordEphSpec = {GRHEAP_EPHEMERON, 0, 8, 16, true,
    nullptr};
const GRHEAP_WeakSpec kQuietSpec = {GRHEAP_WEAK_CELL, offsetof(WeakCell, target),
    0, 0, false, nullptr};

const GRHEAP_Type kWeakCell = {"weak-cell", sizeof(WeakCell), weak_trace,
    cell_destroy, &kCellSpec, nullptr, nullptr};
const GRHEAP_Type kQuietCell = {"quiet-cell", sizeof(WeakCell), weak_trace,
    cell_destroy, &kQuietSpec, nullptr, nullptr};
const GRHEAP_Type kEph = {"ephemeron", sizeof(Eph), nullptr, nullptr, &kEphSpec, nullptr, nullptr};
const GRHEAP_Type kWordCell = {"word-cell", sizeof(WordCell), nullptr, nullptr,
    &kWordSpec, nullptr, nullptr};

/* A target to point at: a Node with a destroy hook that logs its id. */
Node * target(HeapWorld & w, uint64_t id) { return new_node(w.heap, id); }

WeakCell * new_cell(HeapWorld & w, uint64_t id, const GRHEAP_Type * t = &kWeakCell) {
  void * p = nullptr;
  EXPECT_EQ(grheap_alloc(w.heap, t, &p), GRHEAP_OK);
  auto * c = static_cast<WeakCell *>(p);
  c->id = id;
  return c;
}

Eph * new_eph(HeapWorld & w, uint64_t id) {
  void * p = nullptr;
  EXPECT_EQ(grheap_alloc(w.heap, &kEph, &p), GRHEAP_OK);
  auto * e = static_cast<Eph *>(p);
  e->id = id;
  return e;
}

/* Keeps an object alive with a handle; torture-proof from the first line. */
GRHEAP_Handle * hold(HeapWorld & w, void * p) {
  GRHEAP_Handle * h = nullptr;
  EXPECT_EQ(grheap_handle_create(w.heap, p, &h), GRHEAP_OK);
  return h;
}

} // namespace

class WeakTest : public ::testing::Test {
 protected:
  void SetUp() override {
    destroyed_log().clear();
    events().clear();
  }
};

TEST_F(WeakTest, ADeadTargetClearsTheSlotAndTheCallbackRunsOnceAfterTheSweep) {
  HeapWorld w;
  the_heap = w.heap;
  WeakCell * cell = new_cell(w, 10);
  GRHEAP_Handle * hc = hold(w, cell);
  Node * t = target(w, 1);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, t), GRHEAP_OK);
  ASSERT_TRUE(grheap_contains(w.heap, t)); // not collected away early? (torture
                                           // would have: the store above is
                                           // the only thing holding it)
  EXPECT_TRUE(destroyed_log().empty() || destroyed(1));
  events().clear();
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, nullptr);
  ASSERT_EQ(events().size(), 1u);
  const Event & e = events()[0];
  EXPECT_EQ(e.id, 10u);
  EXPECT_EQ(e.slot_then, nullptr);
  EXPECT_TRUE(destroyed(1));
  // After the sweep: the target's destroy hook had run when the callback did.
  EXPECT_EQ(e.destroyed_then, 1u);
  // With the heap busy: allocation and collection are refused, and the cell is
  // live and readable.
  EXPECT_EQ(e.alloc, GRHEAP_ERR_INVALID);
  EXPECT_EQ(e.collect, GRHEAP_ERR_INVALID);
  EXPECT_TRUE(e.live_then);
  EXPECT_EQ(w.stats().weak_cleared, 1u);
  // Once only: nothing more to clear.
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(events().size(), 1u);
  EXPECT_EQ(w.stats().weak_cleared, 1u);
  ASSERT_EQ(grheap_handle_destroy(hc), GRHEAP_OK);
}

TEST_F(WeakTest, ALiveTargetKeepsTheSlotAndTheCallbackIsNotCalled) {
  HeapWorld w;
  the_heap = w.heap;
  WeakCell * cell = new_cell(w, 10);
  GRHEAP_Handle * hc = hold(w, cell);
  Node * t = target(w, 1);
  GRHEAP_Handle * ht = hold(w, t);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, t), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, t);
  EXPECT_TRUE(events().empty());
  // The target held only through the cell's strong slot is also kept.
  Node * t2 = target(w, 2);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->a, t2), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, t2), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, t2);
  EXPECT_TRUE(events().empty());
  EXPECT_EQ(w.stats().weak_cleared, 0u);
  ASSERT_EQ(grheap_handle_destroy(ht), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(hc), GRHEAP_OK);
}

TEST_F(WeakTest, APinnedTargetStaysAndAnUnpinnedOneGoes) {
  HeapWorld w;
  the_heap = w.heap;
  WeakCell * cell = new_cell(w, 10);
  GRHEAP_Handle * hc = hold(w, cell);
  Node * t = target(w, 1);
  ASSERT_EQ(grheap_pin(w.heap, t), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, t), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, t);
  EXPECT_TRUE(events().empty());
  ASSERT_EQ(grheap_unpin(w.heap, t), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, nullptr);
  EXPECT_EQ(events().size(), 1u);
  ASSERT_EQ(grheap_handle_destroy(hc), GRHEAP_OK);
}

TEST_F(WeakTest, ATargetFoundByAConservativeRangeStays) {
  HeapWorld w;
  the_heap = w.heap;
  WeakCell * cell = new_cell(w, 10);
  GRHEAP_Handle * hc = hold(w, cell);
  Node * t = target(w, 1);
  // An interior pointer in a conservative range keeps the object.
  uint64_t word = addr(t) + 8;
  HeldRoots held;
  held.ranges.push_back(range_of(&word, 1));
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held),
      GRCORE_OK);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, t), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, t);
  EXPECT_TRUE(events().empty());
  ASSERT_EQ(grcore_context_remove_root_source(w.ctx, &kHeldSource, &held),
      GRCORE_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, nullptr);
  EXPECT_EQ(events().size(), 1u);
  ASSERT_EQ(grheap_handle_destroy(hc), GRHEAP_OK);
}

TEST_F(WeakTest, ACellThatIsItselfDeadIsDroppedWithoutACallback) {
  HeapWorld w;
  the_heap = w.heap;
  WeakCell * cell = new_cell(w, 10);
  Node * t = target(w, 1);
  (void)t;
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, t), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(events().empty());
  EXPECT_TRUE(destroyed(10));
  EXPECT_EQ(w.stats().weak_cleared, 0u);
  // And the registry did not keep a dangling entry: another collection and a
  // new cell in the same place are fine.
  WeakCell * again = new_cell(w, 11);
  GRHEAP_Handle * h = hold(w, again);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(events().empty());
  ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
}

TEST_F(WeakTest, ACellWithNoCallbackStillClears) {
  HeapWorld w;
  WeakCell * cell = new_cell(w, 10, &kQuietCell);
  GRHEAP_Handle * hc = hold(w, cell);
  Node * t = target(w, 1);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, t), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, nullptr);
  EXPECT_EQ(w.stats().weak_cleared, 1u);
  ASSERT_EQ(grheap_handle_destroy(hc), GRHEAP_OK);
}

TEST_F(WeakTest, AnEphemeronKeepsItsValueWhileTheKeyIsReachableElsewhere) {
  HeapWorld w;
  the_heap = w.heap;
  Eph * e = new_eph(w, 20);
  GRHEAP_Handle * he = hold(w, e);
  Node * key = target(w, 1);
  GRHEAP_Handle * hk = hold(w, key);
  Node * value = target(w, 2); // reachable only through the ephemeron
  ASSERT_EQ(grheap_store(w.heap, e, &e->key, key), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, e, &e->value, value), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, value));
  EXPECT_FALSE(destroyed(2));
  EXPECT_EQ(e->key, key);
  EXPECT_EQ(e->value, value);
  EXPECT_TRUE(events().empty());
  // The key goes: key and value are cleared and freed, the ephemeron reported.
  ASSERT_EQ(grheap_handle_destroy(hk), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(e->key, nullptr);
  EXPECT_EQ(e->value, nullptr);
  EXPECT_TRUE(destroyed(1));
  EXPECT_TRUE(destroyed(2));
  ASSERT_EQ(events().size(), 1u);
  EXPECT_EQ(events()[0].id, 20u);
  EXPECT_EQ(w.stats().ephemerons_cleared, 1u);
  EXPECT_EQ(w.stats().weak_cleared, 0u);
  ASSERT_EQ(grheap_handle_destroy(he), GRHEAP_OK);
}

TEST_F(WeakTest, AValueThatPointsBackAtItsKeyDoesNotKeepEitherAlive) {
  HeapWorld w;
  the_heap = w.heap;
  Eph * e = new_eph(w, 20);
  GRHEAP_Handle * he = hold(w, e);
  Node * key = target(w, 1);
  GRHEAP_Handle * hk = hold(w, key);
  Node * value = target(w, 2);
  ASSERT_EQ(grheap_store(w.heap, value, &value->a, key), GRHEAP_OK); // value -> key
  ASSERT_EQ(grheap_store(w.heap, e, &e->key, key), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, e, &e->value, value), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, key));
  EXPECT_TRUE(grheap_contains(w.heap, value));
  ASSERT_EQ(grheap_handle_destroy(hk), GRHEAP_OK); // only the cycle is left
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(destroyed(1));
  EXPECT_TRUE(destroyed(2));
  EXPECT_EQ(e->key, nullptr);
  EXPECT_EQ(e->value, nullptr);
  EXPECT_EQ(events().size(), 1u);
  EXPECT_EQ(w.stats().ephemerons_cleared, 1u);
  ASSERT_EQ(grheap_handle_destroy(he), GRHEAP_OK);
}

TEST_F(WeakTest, AChainOfEphemeronsIsFollowedToAFixpoint) {
  HeapWorld w;
  the_heap = w.heap;
  // E[i].key = V[i], E[i].value = V[i+1]; V[0] is the only thing held. The
  // ephemerons are allocated last-link first, so the registry order is the
  // worst for a single pass.
  constexpr int kLinks = 6;
  Eph * e[kLinks];
  GRHEAP_Handle * he[kLinks];
  for (int i = kLinks - 1; i >= 0; i--) {
    e[i] = new_eph(w, 100 + static_cast<uint64_t>(i));
    he[i] = hold(w, e[i]);
  }
  Node * v[kLinks + 1];
  GRHEAP_Handle * hv0 = nullptr;
  for (int i = 0; i <= kLinks; i++) {
    v[i] = target(w, static_cast<uint64_t>(i));
    if (i == 0) {
      hv0 = hold(w, v[0]);
    }
    // Later links must stay alive until stored: chain them through the
    // ephemerons as they are made.
    if (i > 0) {
      ASSERT_EQ(grheap_store(w.heap, e[i - 1], &e[i - 1]->value, v[i]),
          GRHEAP_OK);
    }
    if (i < kLinks) {
      ASSERT_EQ(grheap_store(w.heap, e[i], &e[i]->key, v[i]), GRHEAP_OK);
    }
  }
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  for (int i = 0; i <= kLinks; i++) {
    EXPECT_FALSE(destroyed(static_cast<uint64_t>(i))) << i;
  }
  EXPECT_TRUE(events().empty());
  EXPECT_EQ(w.stats().ephemerons_cleared, 0u);
  // Let go of the head of the chain: every link goes.
  ASSERT_EQ(grheap_handle_destroy(hv0), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  for (int i = 0; i <= kLinks; i++) {
    EXPECT_TRUE(destroyed(static_cast<uint64_t>(i))) << i;
  }
  EXPECT_EQ(w.stats().ephemerons_cleared, static_cast<uint64_t>(kLinks));
  EXPECT_EQ(events().size(), static_cast<size_t>(kLinks));
  for (int i = 0; i < kLinks; i++) {
    ASSERT_EQ(grheap_handle_destroy(he[i]), GRHEAP_OK);
  }
}

TEST_F(WeakTest, AnEncodedWeakCellDecodesItsWordAndLeavesAnIntegerAlone) {
  WorldConfig config;
  config.codec_set = true;
  config.codec = GRHEAP_ValueCodec{1, 1, ~UINT64_C(1), 1, 0};
  HeapWorld w(config);
  the_heap = w.heap;
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &kWordCell, &p), GRHEAP_OK);
  auto * c = static_cast<WordCell *>(p);
  c->id = 30;
  GRHEAP_Handle * hc = hold(w, c);
  Node * t = target(w, 1);
  GRHEAP_Handle * ht = hold(w, t);
  ASSERT_EQ(grheap_store_word(w.heap, c, &c->target, (addr(t) << 1) | 1u),
      GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(c->target, (addr(t) << 1) | 1u); // live: kept
  EXPECT_TRUE(events().empty());
  // An integer (tag clear) is not a pointer: neither cleared nor invalid.
  ASSERT_EQ(grheap_store_word(w.heap, c, &c->target, 42u << 1), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(c->target, 42u << 1);
  EXPECT_TRUE(events().empty());
  EXPECT_EQ(w.stats().invalid_pointers, 0u);
  // The target goes: the encoded word is zeroed.
  ASSERT_EQ(grheap_store_word(w.heap, c, &c->target, (addr(t) << 1) | 1u),
      GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(ht), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(c->target, 0u);
  EXPECT_EQ(events().size(), 1u);
  ASSERT_EQ(grheap_handle_destroy(hc), GRHEAP_OK);
}

TEST_F(WeakTest, AnEphemeronWithAnIntegerKeyKeepsItsValue) {
  WorldConfig config;
  config.codec_set = true;
  config.codec = GRHEAP_ValueCodec{1, 1, ~UINT64_C(1), 1, 0};
  HeapWorld w(config);
  static const GRHEAP_Type word_eph = {"word-eph", 24, nullptr, nullptr,
      &kWordEphSpec, nullptr, nullptr};
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &word_eph, &p), GRHEAP_OK);
  auto * words = static_cast<uint64_t *>(p);
  GRHEAP_Handle * h = hold(w, p);
  Node * v = target(w, 1);
  ASSERT_EQ(grheap_store_word(w.heap, p, &words[1], 7u << 1), GRHEAP_OK); // key
  ASSERT_EQ(grheap_store_word(w.heap, p, &words[2], (addr(v) << 1) | 1u),
      GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(destroyed(1));
  EXPECT_EQ(words[2], (addr(v) << 1) | 1u);
  EXPECT_EQ(w.stats().ephemerons_cleared, 0u);
  ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
}

TEST_F(WeakTest, ASlotThatIsNotAPayloadStartIsCountedAndCleared) {
  HeapWorld w;
  the_heap = w.heap;
  WeakCell * cell = new_cell(w, 10);
  GRHEAP_Handle * hc = hold(w, cell);
  int local = 0;
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, &local), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, nullptr);
  EXPECT_EQ(w.stats().invalid_pointers, 1u);
  EXPECT_EQ(events().size(), 1u);
  // An interior pointer is no better.
  Node * t = target(w, 1);
  GRHEAP_Handle * ht = hold(w, t);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target,
                reinterpret_cast<char *>(t) + 8),
      GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, nullptr);
  EXPECT_EQ(w.stats().invalid_pointers, 2u);
  ASSERT_EQ(grheap_handle_destroy(ht), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(hc), GRHEAP_OK);
}

TEST_F(WeakTest, EphemeronSlotsThatAreInvalidOrHaveNoKeyAreCountedAndCleared) {
  HeapWorld w(steady());
  the_heap = w.heap;
  int local = 0;
  Node * live = target(w, 1);
  GRHEAP_Handle * hl = hold(w, live);
  // 1: the key is not a payload start (the value is a live object).
  Eph * bad_key = new_eph(w, 21);
  GRHEAP_Handle * h1 = hold(w, bad_key);
  ASSERT_EQ(grheap_store(w.heap, bad_key, &bad_key->key, &local), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, bad_key, &bad_key->value, live), GRHEAP_OK);
  // 2: the key is live and the value is not a payload start.
  Eph * bad_value = new_eph(w, 22);
  GRHEAP_Handle * h2 = hold(w, bad_value);
  ASSERT_EQ(grheap_store(w.heap, bad_value, &bad_value->key, live), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, bad_value, &bad_value->value, &local),
      GRHEAP_OK);
  // 3: no key, but a value (written in the wrong order).
  Eph * no_key = new_eph(w, 23);
  GRHEAP_Handle * h3 = hold(w, no_key);
  ASSERT_EQ(grheap_store(w.heap, no_key, &no_key->value, live), GRHEAP_OK);
  events().clear();
  uint64_t invalid_before = w.stats().invalid_pointers;
  uint64_t cleared_before = w.stats().ephemerons_cleared;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(bad_key->key, nullptr);
  EXPECT_EQ(bad_key->value, nullptr);
  EXPECT_EQ(bad_value->key, live); // the live key stays
  EXPECT_EQ(bad_value->value, nullptr);
  EXPECT_EQ(no_key->key, nullptr);
  EXPECT_EQ(no_key->value, nullptr);
  EXPECT_EQ(w.stats().invalid_pointers - invalid_before, 2u);
  EXPECT_EQ(w.stats().ephemerons_cleared - cleared_before, 3u);
  ASSERT_EQ(events().size(), 3u);
  // A second collection has nothing left to clear or report.
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(events().size(), 3u);
  EXPECT_EQ(w.stats().ephemerons_cleared - cleared_before, 3u);
  ASSERT_EQ(grheap_handle_destroy(h3), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(h2), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(h1), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(hl), GRHEAP_OK);
}

TEST_F(WeakTest, BarrierVerifyWatchesTheEphemeronSlotsToo) {
  WorldConfig config;
  config.verify = GRHEAP_VERIFY_COUNT;
  HeapWorld w(config);
  Eph * e = new_eph(w, 20);
  GRHEAP_Handle * he = hold(w, e);
  Node * k = target(w, 1);
  GRHEAP_Handle * hk = hold(w, k);
  uint64_t violations = 99;
  ASSERT_EQ(grheap_verify(w.heap, &violations), GRHEAP_OK);
  EXPECT_EQ(violations, 0u);
  e->key = k; // direct write to the key slot
  ASSERT_EQ(grheap_verify(w.heap, &violations), GRHEAP_OK);
  EXPECT_GE(violations, 1u);
  e->key = nullptr;
  e->value = k; // and to the value slot
  ASSERT_EQ(grheap_verify(w.heap, &violations), GRHEAP_OK);
  EXPECT_GE(violations, 1u);
  e->value = nullptr;
  ASSERT_EQ(grheap_verify(w.heap, &violations), GRHEAP_OK);
  EXPECT_EQ(violations, 0u);
  ASSERT_EQ(grheap_handle_destroy(hk), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(he), GRHEAP_OK);
}

TEST_F(WeakTest, ASpecWhoseSlotsDoNotFitThePayloadIsRefusedAtAllocation) {
  HeapWorld w;
  GRHEAP_Stats before = w.stats();
  // A sized allocation too small for the weak slot.
  void * p = reinterpret_cast<void *>(0x1);
  EXPECT_EQ(grheap_alloc_sized(w.heap, &kWeakCell, 8, &p), GRHEAP_ERR_INVALID);
  EXPECT_EQ(p, reinterpret_cast<void *>(0x1));
  // A misaligned offset, an ephemeron with one slot twice, and a bad kind.
  static const GRHEAP_WeakSpec misaligned = {GRHEAP_WEAK_CELL, 4, 0, 0, false,
      nullptr};
  static const GRHEAP_WeakSpec same = {GRHEAP_EPHEMERON, 0, 8, 8, false, nullptr};
  static const GRHEAP_WeakSpec bad = {static_cast<GRHEAP_WeakKind>(7), 0, 0, 0,
      false, nullptr};
  const GRHEAP_Type t1 = {"m", 32, nullptr, nullptr, &misaligned, nullptr, nullptr};
  const GRHEAP_Type t2 = {"s", 32, nullptr, nullptr, &same, nullptr, nullptr};
  const GRHEAP_Type t3 = {"b", 32, nullptr, nullptr, &bad, nullptr, nullptr};
  // Ephemeron slots: the value slot starting at the payload's end, and a
  // misaligned key and value.
  static const GRHEAP_WeakSpec at_end = {GRHEAP_EPHEMERON, 0, 0, 32, false,
      nullptr};
  static const GRHEAP_WeakSpec odd_key = {GRHEAP_EPHEMERON, 0, 4, 8, false,
      nullptr};
  static const GRHEAP_WeakSpec odd_value = {GRHEAP_EPHEMERON, 0, 0, 12, false,
      nullptr};
  const GRHEAP_Type t4 = {"e", 32, nullptr, nullptr, &at_end, nullptr, nullptr};
  const GRHEAP_Type t5 = {"k", 32, nullptr, nullptr, &odd_key, nullptr, nullptr};
  const GRHEAP_Type t6 = {"v", 32, nullptr, nullptr, &odd_value, nullptr, nullptr};
  EXPECT_EQ(grheap_alloc(w.heap, &t4, &p), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_alloc(w.heap, &t5, &p), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_alloc(w.heap, &t6, &p), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_alloc(w.heap, &t1, &p), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_alloc(w.heap, &t2, &p), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_alloc(w.heap, &t3, &p), GRHEAP_ERR_INVALID);
  EXPECT_EQ(w.stats().allocations, before.allocations);
  EXPECT_EQ(w.stats().live_objects, before.live_objects);
  // The same type at a size that fits is fine.
  EXPECT_EQ(grheap_alloc_sized(w.heap, &kWeakCell, 64, &p), GRHEAP_OK);
}

TEST_F(WeakTest, ManyCellsAreEachClearedAndReportedOnce) {
  HeapWorld w;
  the_heap = w.heap;
  constexpr int kCells = 100; // past the registry's first growth
  GRHEAP_Handle * handles[kCells];
  for (int i = 0; i < kCells; i++) {
    WeakCell * c = new_cell(w, 1000 + static_cast<uint64_t>(i));
    handles[i] = hold(w, c);
    Node * t = target(w, static_cast<uint64_t>(i));
    ASSERT_EQ(grheap_store(w.heap, c, &c->target, t), GRHEAP_OK);
  }
  events().clear(); // a torture collection may already have reported some
  uint64_t before = w.stats().weak_cleared;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().weak_cleared - before, events().size());
  EXPECT_EQ(w.stats().weak_cleared, static_cast<uint64_t>(kCells));
  for (int i = 0; i < kCells; i++) {
    EXPECT_TRUE(destroyed(static_cast<uint64_t>(i))) << i;
    ASSERT_EQ(grheap_handle_destroy(handles[i]), GRHEAP_OK);
  }
}

TEST_F(WeakTest, AnArenaAllocatesWeakCellsAndNeverClearsThem) {
  WorldConfig config;
  config.mode = GRHEAP_MODE_ARENA;
  HeapWorld w(config);
  the_heap = w.heap;
  WeakCell * cell = new_cell(w, 10);
  Node * t = target(w, 1);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, t), GRHEAP_OK);
  Eph * e = new_eph(w, 20);
  ASSERT_EQ(grheap_store(w.heap, e, &e->key, t), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, t);
  EXPECT_EQ(e->key, t);
  EXPECT_TRUE(events().empty());
  EXPECT_EQ(w.stats().weak_cleared, 0u);
  ASSERT_EQ(grheap_arena_reset(w.heap), GRHEAP_OK);
  EXPECT_TRUE(destroyed(10));
  EXPECT_TRUE(destroyed(1));
  EXPECT_TRUE(events().empty());
  // The arena is reusable afterwards.
  WeakCell * again = new_cell(w, 11);
  EXPECT_NE(again, nullptr);
}

TEST_F(WeakTest, ACollectionFromInsideTheCallbackIsRefusedAndTheHeapSurvives) {
  HeapWorld w;
  the_heap = w.heap;
  WeakCell * cell = new_cell(w, 10);
  GRHEAP_Handle * hc = hold(w, cell);
  Node * t = target(w, 1);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, t), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  ASSERT_EQ(events().size(), 1u);
  EXPECT_EQ(events()[0].collect, GRHEAP_ERR_INVALID);
  // The refused attempts changed nothing: allocation works again.
  Node * n = target(w, 5);
  EXPECT_NE(n, nullptr);
  ASSERT_EQ(grheap_handle_destroy(hc), GRHEAP_OK);
}

TEST_F(WeakTest, TheCollectorsClearsAreKnownToBarrierVerify) {
  WorldConfig config;
  config.verify = GRHEAP_VERIFY_COUNT;
  HeapWorld w(config);
  the_heap = w.heap;
  WeakCell * cell = new_cell(w, 10);
  GRHEAP_Handle * hc = hold(w, cell);
  Eph * e = new_eph(w, 20);
  GRHEAP_Handle * he = hold(w, e);
  Node * t = target(w, 1);
  Node * k = target(w, 2);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, t), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, e, &e->key, k), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, e, &e->value, t), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(cell->target, nullptr);
  EXPECT_EQ(e->key, nullptr);
  uint64_t violations = 99;
  ASSERT_EQ(grheap_verify(w.heap, &violations), GRHEAP_OK);
  EXPECT_EQ(violations, 0u);
  EXPECT_EQ(w.stats().verify_violations, 0u);
  // A direct write to the weak slot is found.
  cell->target = t;
  ASSERT_EQ(grheap_verify(w.heap, &violations), GRHEAP_OK);
  EXPECT_GE(violations, 1u);
  cell->target = nullptr;
  ASSERT_EQ(grheap_handle_destroy(he), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_destroy(hc), GRHEAP_OK);
}

/* ---- Allocation failure: the registry grows before the cell is taken ----- */

TEST_F(WeakTest, AFailedRegistryGrowthLeavesNothingAllocated) {
  HeapWorld w(steady());
  // Fill the registry to its first capacity (16), so the next allocation must
  // grow it; fail that one.
  GRHEAP_Handle * hs[16];
  for (int i = 0; i < 16; i++) {
    hs[i] = hold(w, new_cell(w, static_cast<uint64_t>(i)));
  }
  GRHEAP_Stats before = w.stats();
  w.allocator.fail_all = true;
  void * p = reinterpret_cast<void *>(0x1);
  EXPECT_EQ(grheap_alloc(w.heap, &kWeakCell, &p), GRHEAP_ERR_OOM);
  w.allocator.fail_all = false;
  EXPECT_EQ(p, reinterpret_cast<void *>(0x1));
  GRHEAP_Stats after = w.stats();
  EXPECT_EQ(after.allocations, before.allocations);
  EXPECT_EQ(after.live_objects, before.live_objects);
  EXPECT_EQ(after.collections, before.collections); // not even a collection
  // And it works again.
  EXPECT_EQ(grheap_alloc(w.heap, &kWeakCell, &p), GRHEAP_OK);
  for (GRHEAP_Handle * h : hs) {
    ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
  }
}

TEST_F(WeakTest, EveryNthAllocatorFailureWhileMakingWeakCellsIsSurvived) {
  for (long n = 1; n < 40; n++) {
    HeapWorld w(steady());
    GRHEAP_Handle * hs[40];
    int made = 0;
    w.allocator.calls = 0;
    w.allocator.fail_at = n;
    w.allocator.sticky = true;
    int failures = 0;
    for (int i = 0; i < 40; i++) {
      void * p = nullptr;
      GRHEAP_Result r = grheap_alloc(w.heap, &kWeakCell, &p);
      if (r != GRHEAP_OK) {
        ASSERT_EQ(r, GRHEAP_ERR_OOM) << "N=" << n;
        EXPECT_EQ(p, nullptr);
        failures++;
        continue;
      }
      GRHEAP_Handle * h = nullptr;
      if (grheap_handle_create(w.heap, p, &h) == GRHEAP_OK) {
        hs[made++] = h;
      }
    }
    w.allocator.fail_at = 0;
    w.allocator.sticky = false;
    if (n <= 5) {
      EXPECT_GT(failures, 0) << "N=" << n; // the first allocations all fail
    }
    // Whatever was made is consistent: a collection runs and keeps the held.
    uint64_t live = w.stats().live_objects;
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    EXPECT_LE(w.stats().live_objects, live);
    for (int i = 0; i < made; i++) {
      ASSERT_EQ(grheap_handle_destroy(hs[i]), GRHEAP_OK);
    }
  }
}

TEST_F(WeakTest, TheRegistryAndTheCellsMigrateWithTheContext) {
  HeapWorld w;
  the_heap = w.heap;
  WeakCell * cell = new_cell(w, 10);
  GRHEAP_Handle * hc = hold(w, cell);
  Node * t = target(w, 1);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, t), GRHEAP_OK);
  ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  std::thread other([&] {
    ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    ASSERT_EQ(grcore_context_release(w.ctx), GRCORE_OK);
  });
  other.join();
  ASSERT_EQ(grcore_context_acquire(w.ctx), GRCORE_OK);
  EXPECT_EQ(cell->target, nullptr);
  EXPECT_EQ(events().size(), 1u);
  ASSERT_EQ(grheap_handle_destroy(hc), GRHEAP_OK);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
