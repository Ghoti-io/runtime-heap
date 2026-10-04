/**
 * @file
 *
 * The retention query (retention.h): the shortest chain of references from a
 * root to an object, for every kind of root, through weak cells and
 * ephemerons, and what it refuses; and that it changes nothing.
 *
 * "Changes nothing" is checked against the heap's own state, not its public
 * statistics alone: the fingerprint below covers every span's allocation, mark
 * and pin bits, the free lists, the mark stack's bookkeeping, the counters and
 * the ID tables, so a mark bit set and not cleared, or a counter bumped, fails.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

/* ---- Types ------------------------------------------------------------------ */

struct WeakCell {
  void * a;
  void * b;
  uint64_t id;
  void * target;
};
const GRHEAP_WeakSpec kCellSpec = {GRHEAP_WEAK_CELL, offsetof(WeakCell, target),
    0, 0, false, nullptr};
void weak_trace(GRHEAP_Tracer * t, void * payload) {
  auto * c = static_cast<WeakCell *>(payload);
  grheap_trace_slot(t, &c->a);
  grheap_trace_slot(t, &c->b);
}
const GRHEAP_Type kWeakCell = {"weak-cell", sizeof(WeakCell), weak_trace, nullptr,
    &kCellSpec, nullptr, nullptr};

struct Eph {
  uint64_t id;
  void * key;
  void * value;
};
const GRHEAP_WeakSpec kEphSpec = {GRHEAP_EPHEMERON, 0, offsetof(Eph, key),
    offsetof(Eph, value), false, nullptr};
const GRHEAP_Type kEph = {"ephemeron", sizeof(Eph), nullptr, nullptr, &kEphSpec,
    nullptr, nullptr};

/* The same with codec-encoded slots, for an immediate key. */
const GRHEAP_WeakSpec kWordEphSpec = {GRHEAP_EPHEMERON, 0, offsetof(Eph, key),
    offsetof(Eph, value), true, nullptr};
const GRHEAP_Type kWordEph = {"word-ephemeron", sizeof(Eph), nullptr, nullptr,
    &kWordEphSpec, nullptr, nullptr};
/* Low bit set: an immediate. Low bit clear: a pointer. */
const GRHEAP_ValueCodec kTagCodec = {1, 0, ~UINT64_C(1), 0, 0};

/* A node whose slots hold codec words (so a retention walk takes the encoded
 * path through the tracer). */
struct WordNode {
  uint64_t a;
  uint64_t b;
  uint64_t id;
};
void word_trace(GRHEAP_Tracer * t, void * payload) {
  auto * n = static_cast<WordNode *>(payload);
  grheap_trace_word(t, &n->a);
  grheap_trace_word(t, &n->b);
}
const GRHEAP_Type kWordNode = {"word-node", sizeof(WordNode), word_trace,
    nullptr, nullptr, nullptr, nullptr};

/* ---- Helpers ---------------------------------------------------------------- */

constexpr size_t kA = offsetof(Node, a);
constexpr size_t kB = offsetof(Node, b);

Node * mk(GRHEAP_Heap * heap, uint64_t id, const GRHEAP_Type * t = &kPlainNode) {
  return new_node(heap, id, t);
}

void link_a(HeapWorld & w, Node * from, void * to) {
  ASSERT_EQ(grheap_store(w.heap, from, &from->a, to), GRHEAP_OK);
}
void link_b(HeapWorld & w, Node * from, void * to) {
  ASSERT_EQ(grheap_store(w.heap, from, &from->b, to), GRHEAP_OK);
}

/* A result that is released when it goes out of scope. */
struct Path {
  GRHEAP_Retention * p = nullptr;
  Path() = default;
  Path(const Path &) = delete;
  Path & operator=(const Path &) = delete;
  ~Path() { grheap_retention_release(p); }
  GRHEAP_Result ask(GRHEAP_Heap * heap, const void * object,
      const GRHEAP_Allocator * allocator = nullptr) {
    grheap_retention_release(p);
    p = nullptr;
    return grheap_retention_path(heap, object, allocator, &p);
  }
  bool retained() const { return grheap_retention_retained(p); }
  size_t count() const { return grheap_retention_step_count(p); }
  GRHEAP_RetentionRoot root() const {
    GRHEAP_RetentionRoot r{};
    EXPECT_EQ(grheap_retention_root(p, &r), GRHEAP_OK);
    return r;
  }
  GRHEAP_RetentionStep step(size_t i) const {
    GRHEAP_RetentionStep s{};
    EXPECT_EQ(grheap_retention_step(p, i, &s), GRHEAP_OK);
    return s;
  }
};

/* Expects the steps' objects, and each step's slot offset except the last. */
void expect_chain(const Path & path, std::vector<const void *> objects,
    std::vector<size_t> offsets) {
  ASSERT_TRUE(path.retained());
  ASSERT_EQ(path.count(), objects.size());
  ASSERT_EQ(offsets.size() + 1, objects.size());
  for (size_t i = 0; i < objects.size(); i++) {
    GRHEAP_RetentionStep s = path.step(i);
    EXPECT_EQ(s.object, objects[i]) << "step " << i;
    EXPECT_EQ(s.target, i + 1 == objects.size()) << "step " << i;
    EXPECT_EQ(s.ephemeron, nullptr) << "step " << i;
    if (i + 1 < objects.size()) {
      EXPECT_EQ(s.slot_offset, offsets[i]) << "step " << i;
    } else {
      EXPECT_EQ(s.slot_offset, 0u);
    }
  }
}

/* Everything the query must not change, read from the heap itself: a digest
 * of every span's allocation, mark and pin bits, free lists and counters, the
 * mark stack's bookkeeping and the ID tables' sizes (src/heap/digest.c, which
 * the archive the tests link exposes). The public statistics go in beside it,
 * so a failure names which side moved. */
extern "C" uint64_t grheap_state_digest(const GRHEAP_Heap * heap);

std::vector<uint64_t> fingerprint(GRHEAP_Heap * h) {
  GRHEAP_Stats stats{};
  EXPECT_EQ(grheap_stats(h, &stats), GRHEAP_OK);
  std::vector<uint64_t> f;
  f.push_back(grheap_state_digest(h));
  const unsigned char * bytes = reinterpret_cast<const unsigned char *>(&stats);
  for (size_t i = 0; i < sizeof stats; i++) {
    f.push_back(bytes[i]);
  }
  return f;
}

/* A world with the chain root -> A -> B -> L held by whatever the test says. */
struct Chain {
  Node * a;
  Node * b;
  Node * leaked;
};
Chain build_chain(HeapWorld & w) {
  Chain c{};
  c.a = mk(w.heap, 1);
  c.b = mk(w.heap, 2);
  c.leaked = mk(w.heap, 3);
  link_a(w, c.a, c.b);       // A.a -> B   (offset 0)
  link_b(w, c.b, c.leaked);  // B.b -> L   (offset 8)
  return c;
}

} // namespace

/* ---- Each kind of root --------------------------------------------------------- */

TEST(Retention, AContextRootSourceSlotIsNamedWithTheSourceAndTheSlotsOrdinal) {
  HeapWorld w(steady());
  Chain c = build_chain(w);
  uint64_t other0 = 0, other1 = 0, slot = addr(c.a);
  HeldRoots held;
  held.slots = {&other0, &other1, &slot};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held), GRCORE_OK);

  Path path;
  ASSERT_EQ(path.ask(w.heap, c.leaked), GRHEAP_OK);
  GRHEAP_RetentionRoot root = path.root();
  EXPECT_EQ(root.kind, GRHEAP_ROOT_CONTEXT_SOURCE);
  EXPECT_STREQ(root.source, "held");
  EXPECT_EQ(root.ordinal, 2u);
  EXPECT_EQ(root.offset, 0u);
  expect_chain(path, {c.a, c.b, c.leaked}, {kA, kB});
  EXPECT_EQ(path.step(0).type, &kPlainNode);
  EXPECT_STREQ(path.step(2).type->name, "plain");
}

TEST(Retention, ASecondSourcesSlotsAreNumberedFromZeroUnderThatSourcesName) {
  HeapWorld w(steady());
  Chain c = build_chain(w);
  static const GRCORE_RootSource kOther = {"other", held_enumerate};
  uint64_t first = 0, second = addr(c.a);
  HeldRoots h1, h2;
  h1.slots = {&first};
  h2.slots = {&second};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &h1), GRCORE_OK);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kOther, &h2), GRCORE_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, c.leaked), GRHEAP_OK);
  EXPECT_STREQ(path.root().source, "other");
  EXPECT_EQ(path.root().ordinal, 0u);
}

TEST(Retention, ARegisteredCRootIsNamedByItsRegistrationOrder) {
  HeapWorld w(steady());
  Chain c = build_chain(w);
  void * unrelated = mk(w.heap, 9);
  void * through = c.a;
  ASSERT_EQ(grheap_root_add(w.heap, &unrelated), GRHEAP_OK);
  ASSERT_EQ(grheap_root_add(w.heap, &through), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, c.leaked), GRHEAP_OK);
  GRHEAP_RetentionRoot root = path.root();
  EXPECT_EQ(root.kind, GRHEAP_ROOT_C_ROOT);
  EXPECT_EQ(root.source, nullptr);
  EXPECT_EQ(root.ordinal, 1u);
  expect_chain(path, {c.a, c.b, c.leaked}, {kA, kB});
  ASSERT_EQ(grheap_root_remove(w.heap, &unrelated), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &through), GRHEAP_OK);
}

TEST(Retention, AHandleIsNamedByItsPositionInTheCollectorsOrder) {
  HeapWorld w(steady());
  Chain c = build_chain(w);
  GRHEAP_Handle * through = nullptr;
  GRHEAP_Handle * later = nullptr;
  ASSERT_EQ(grheap_handle_create(w.heap, c.a, &through), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_create(w.heap, mk(w.heap, 9), &later), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, c.leaked), GRHEAP_OK);
  GRHEAP_RetentionRoot root = path.root();
  EXPECT_EQ(root.kind, GRHEAP_ROOT_HANDLE);
  EXPECT_EQ(root.ordinal, 1u); // the newest handle is visited first
  expect_chain(path, {c.a, c.b, c.leaked}, {kA, kB});
  grheap_handle_destroy(later);
  grheap_handle_destroy(through);
}

TEST(Retention, APinnedObjectIsARootOfKindPin) {
  HeapWorld w(steady());
  Chain c = build_chain(w);
  ASSERT_EQ(grheap_pin(w.heap, c.a), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, c.leaked), GRHEAP_OK);
  GRHEAP_RetentionRoot root = path.root();
  EXPECT_EQ(root.kind, GRHEAP_ROOT_PIN);
  EXPECT_EQ(root.ordinal, 0u);
  expect_chain(path, {c.a, c.b, c.leaked}, {kA, kB});
  // The object a pin holds is itself a path of one step.
  Path self;
  ASSERT_EQ(self.ask(w.heap, c.a), GRHEAP_OK);
  EXPECT_EQ(self.root().kind, GRHEAP_ROOT_PIN);
  expect_chain(self, {c.a}, {});
  ASSERT_EQ(grheap_unpin(w.heap, c.a), GRHEAP_OK);
}

TEST(Retention, AWordInAConservativeRangeIsARootWithItsSourceAndByteOffset) {
  HeapWorld w(steady());
  Chain c = build_chain(w);
  // A "stack": the fourth word points into the middle of A. A second, empty
  // range comes first, so the ordinal is not zero.
  uint64_t words[6] = {1, 2, 3, addr(c.a) + 8, 5, 6};
  HeldRoots held;
  held.ranges = {range_of(words, 0), range_of(words, 6)};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held), GRCORE_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, c.leaked), GRHEAP_OK);
  GRHEAP_RetentionRoot root = path.root();
  EXPECT_EQ(root.kind, GRHEAP_ROOT_CONSERVATIVE);
  EXPECT_STREQ(root.source, "held");
  EXPECT_EQ(root.ordinal, 1u);
  EXPECT_EQ(root.offset, 3u * 8u);
  expect_chain(path, {c.a, c.b, c.leaked}, {kA, kB});
}

TEST(Retention, AConservativeWordForWhichTheCodecIsDecodedAddsItsBaseAndShift) {
  HeapWorld w(steady());
  Node * n = mk(w.heap, 1);
  uint64_t word = (addr(n) - 0x40) << 4 | 0xF; // low bits are tag noise
  HeldRoots held;
  GRCORE_ConservativeRange r = range_of(&word, 1);
  r.mask = ~UINT64_C(0xF);
  r.shift = 4;
  r.base = 0x40;
  held.ranges = {r};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held), GRCORE_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, n), GRHEAP_OK);
  ASSERT_TRUE(path.retained());
  EXPECT_EQ(path.root().kind, GRHEAP_ROOT_CONSERVATIVE);
}

TEST(Retention, ATypeThatReportsEncodedWordsIsWalkedThroughTheCodec) {
  WorldConfig config = steady();
  config.codec_set = true;
  config.codec = kTagCodec;
  HeapWorld w(config);
  void * pa = nullptr;
  void * pb = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kWordNode, &pa), GRHEAP_OK);
  ASSERT_EQ(grheap_alloc(w.heap, &kWordNode, &pb), GRHEAP_OK);
  auto * a = static_cast<WordNode *>(pa);
  auto * b = static_cast<WordNode *>(pb);
  ASSERT_EQ(grheap_store_word(w.heap, a, &a->a, 7), GRHEAP_OK); // an immediate
  ASSERT_EQ(grheap_store_word(w.heap, a, &a->b, addr(b)), GRHEAP_OK);
  void * root = a;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, b), GRHEAP_OK);
  expect_chain(path, {a, b}, {offsetof(WordNode, b)});
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

/* ---- Shortest, deterministic, cyclic -------------------------------------------- */

TEST(Retention, TwoRootsByChainsOfThreeAndFiveGiveTheThreeHopChain) {
  HeapWorld w(steady());
  Node * target = mk(w.heap, 100);
  // Long chain: l1 -> l2 -> l3 -> l4 -> target (five objects with target).
  Node * l1 = mk(w.heap, 1);
  Node * l2 = mk(w.heap, 2);
  Node * l3 = mk(w.heap, 3);
  Node * l4 = mk(w.heap, 4);
  link_a(w, l1, l2);
  link_a(w, l2, l3);
  link_b(w, l3, l4);
  link_a(w, l4, target);
  // Short chain: s1 -> s2 -> target.
  Node * s1 = mk(w.heap, 11);
  Node * s2 = mk(w.heap, 12);
  link_b(w, s1, s2);
  link_b(w, s2, target);
  // The long one is registered first, so a depth-blind search would take it.
  void * r_long = l1;
  void * r_short = s1;
  ASSERT_EQ(grheap_root_add(w.heap, &r_long), GRHEAP_OK);
  ASSERT_EQ(grheap_root_add(w.heap, &r_short), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, target), GRHEAP_OK);
  EXPECT_EQ(path.root().ordinal, 1u);
  expect_chain(path, {s1, s2, target}, {kB, kB});
  ASSERT_EQ(grheap_root_remove(w.heap, &r_long), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &r_short), GRHEAP_OK);
}

TEST(Retention, EqualChainsAreBrokenInTheCollectorsRootThenSlotOrder) {
  HeapWorld w(steady());
  Node * target = mk(w.heap, 100);
  Node * via_a = mk(w.heap, 1);
  Node * via_b = mk(w.heap, 2);
  link_a(w, via_a, target);
  link_b(w, via_b, target);
  // One object references the target through both slots: the first slot wins.
  Node * both = mk(w.heap, 3);
  link_a(w, both, target);
  link_b(w, both, target);
  // C root, then handle, then context source: a C root wins over a handle.
  void * c_root = via_a;
  GRHEAP_Handle * handle = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &c_root), GRHEAP_OK);
  ASSERT_EQ(grheap_handle_create(w.heap, via_b, &handle), GRHEAP_OK);
  for (int i = 0; i < 3; i++) {
    Path path;
    ASSERT_EQ(path.ask(w.heap, target), GRHEAP_OK);
    EXPECT_EQ(path.root().kind, GRHEAP_ROOT_C_ROOT) << "run " << i;
    expect_chain(path, {via_a, target}, {kA});
  }
  // Without the C root the handle is the answer; with `both` rooted too, its
  // slot a is the hop (the handle's chain is the same length, and the handle is
  // earlier in the collector's order than a source's slot).
  ASSERT_EQ(grheap_root_remove(w.heap, &c_root), GRHEAP_OK);
  Path via_handle;
  ASSERT_EQ(via_handle.ask(w.heap, target), GRHEAP_OK);
  EXPECT_EQ(via_handle.root().kind, GRHEAP_ROOT_HANDLE);
  expect_chain(via_handle, {via_b, target}, {kB});
  uint64_t slot = addr(both);
  HeldRoots held;
  held.slots = {&slot};
  ASSERT_EQ(grheap_handle_destroy(handle), GRHEAP_OK);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held), GRCORE_OK);
  Path through_both;
  ASSERT_EQ(through_both.ask(w.heap, target), GRHEAP_OK);
  expect_chain(through_both, {both, target}, {kA});
}

TEST(Retention, ACycleIsWalkedOnceAndTheShortestChainIsStillFound) {
  HeapWorld w(steady());
  Node * n1 = mk(w.heap, 1);
  Node * n2 = mk(w.heap, 2);
  Node * n3 = mk(w.heap, 3);
  Node * target = mk(w.heap, 4);
  link_a(w, n1, n2);
  link_a(w, n2, n3);
  link_a(w, n3, n1); // the cycle n1 -> n2 -> n3 -> n1
  link_b(w, n2, n2); // and a self-loop
  link_b(w, n3, target);
  void * root = n1;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, target), GRHEAP_OK);
  expect_chain(path, {n1, n2, n3, target}, {kA, kA, kB});
  // An object in the cycle itself, and the cycle's own root.
  Path in_cycle;
  ASSERT_EQ(in_cycle.ask(w.heap, n3), GRHEAP_OK);
  expect_chain(in_cycle, {n1, n2, n3}, {kA, kA});
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

/* ---- Weak cells and ephemerons ---------------------------------------------------- */

TEST(Retention, AnObjectHeldOnlyByAWeakCellIsNotRetainedAndACollectionFreesIt) {
  HeapWorld w(steady());
  destroyed_log().clear();
  void * pc = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kWeakCell, &pc), GRHEAP_OK);
  auto * cell = static_cast<WeakCell *>(pc);
  Node * target = mk(w.heap, 77, &kNode);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, target), GRHEAP_OK);
  void * root = cell;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, target), GRHEAP_OK);
  EXPECT_FALSE(path.retained());
  EXPECT_EQ(path.count(), 0u);
  GRHEAP_RetentionRoot r;
  EXPECT_EQ(grheap_retention_root(path.p, &r), GRHEAP_ERR_INVALID);
  // What a collection then does agrees with the answer.
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(destroyed(77));
  EXPECT_EQ(cell->target, nullptr);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST(Retention, AWeakCellDoesNotHideAStrongPathToTheSameObject) {
  HeapWorld w(steady());
  void * pc = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kWeakCell, &pc), GRHEAP_OK);
  auto * cell = static_cast<WeakCell *>(pc);
  Node * target = mk(w.heap, 77);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->target, target), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, cell, &cell->b, target), GRHEAP_OK); // strong
  void * root = cell;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, target), GRHEAP_OK);
  ASSERT_TRUE(path.retained());
  EXPECT_EQ(path.step(0).slot_offset, offsetof(WeakCell, b));
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST(Retention, AnEphemeronsValueIsReachedThroughItsKeyAndTheHopIsMarked) {
  HeapWorld w(steady());
  destroyed_log().clear();
  void * pe = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kEph, &pe), GRHEAP_OK);
  auto * eph = static_cast<Eph *>(pe);
  Node * key = mk(w.heap, 1, &kNode);
  Node * value = mk(w.heap, 2, &kNode);
  ASSERT_EQ(grheap_store(w.heap, eph, &eph->key, key), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, eph, &eph->value, value), GRHEAP_OK);
  void * root_eph = eph;
  void * root_key = key;
  ASSERT_EQ(grheap_root_add(w.heap, &root_eph), GRHEAP_OK);
  ASSERT_EQ(grheap_root_add(w.heap, &root_key), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, value), GRHEAP_OK);
  ASSERT_TRUE(path.retained());
  // The chain is the key's root, the key, and a hop to the value.
  EXPECT_EQ(path.root().kind, GRHEAP_ROOT_C_ROOT);
  EXPECT_EQ(path.root().ordinal, 1u);
  ASSERT_EQ(path.count(), 2u);
  GRHEAP_RetentionStep s0 = path.step(0);
  GRHEAP_RetentionStep s1 = path.step(1);
  EXPECT_EQ(s0.object, key);
  EXPECT_EQ(s0.ephemeron, eph);
  EXPECT_EQ(s0.slot_offset, offsetof(Eph, value));
  EXPECT_FALSE(s0.target);
  EXPECT_EQ(s1.object, value);
  EXPECT_TRUE(s1.target);
  EXPECT_EQ(s1.ephemeron, nullptr);
  // And the collector agrees: the value survives while the key does.
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(destroyed(2));
  // Drop the key's root: now neither key nor value is kept, and the query,
  // asked before the collection, says the value is not retained.
  ASSERT_EQ(grheap_root_remove(w.heap, &root_key), GRHEAP_OK);
  Path gone;
  ASSERT_EQ(gone.ask(w.heap, value), GRHEAP_OK);
  EXPECT_FALSE(gone.retained());
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(destroyed(2));
  ASSERT_EQ(grheap_root_remove(w.heap, &root_eph), GRHEAP_OK);
}

TEST(Retention, AnEphemeronNothingReachesKeepsNothing) {
  HeapWorld w(steady());
  void * pe = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kEph, &pe), GRHEAP_OK);
  auto * eph = static_cast<Eph *>(pe);
  Node * key = mk(w.heap, 1);
  Node * value = mk(w.heap, 2);
  ASSERT_EQ(grheap_store(w.heap, eph, &eph->key, key), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, eph, &eph->value, value), GRHEAP_OK);
  void * root_key = key; // the key is live, but the ephemeron is not
  ASSERT_EQ(grheap_root_add(w.heap, &root_key), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, value), GRHEAP_OK);
  EXPECT_FALSE(path.retained());
  ASSERT_EQ(grheap_root_remove(w.heap, &root_key), GRHEAP_OK);
}

TEST(Retention, AnEphemeronWithAnImmediateKeyKeepsItsValueAsLongAsItLives) {
  WorldConfig config = steady();
  config.codec_set = true;
  config.codec = kTagCodec;
  HeapWorld w(config);
  void * pe = nullptr;
  void * pv = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kWordEph, &pe), GRHEAP_OK);
  ASSERT_EQ(grheap_alloc(w.heap, &kLeaf, &pv), GRHEAP_OK);
  auto * eph = static_cast<Eph *>(pe);
  ASSERT_EQ(grheap_store_word(w.heap, eph, reinterpret_cast<uint64_t *>(&eph->key), 5),
      GRHEAP_OK);
  ASSERT_EQ(grheap_store_word(w.heap, eph, reinterpret_cast<uint64_t *>(&eph->value),
                addr(pv)),
      GRHEAP_OK);
  void * root = eph;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, pv), GRHEAP_OK);
  ASSERT_EQ(path.count(), 2u);
  EXPECT_EQ(path.step(0).object, eph);
  EXPECT_EQ(path.step(0).ephemeron, eph); // the ephemeron is its own holder
  EXPECT_EQ(path.step(0).slot_offset, offsetof(Eph, value));
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST(Retention, AValueReachedThroughAChainOfEphemeronsIsExplainedHopByHop) {
  HeapWorld w(steady());
  // e1: key k1 (rooted) -> value k2; e2: key k2 -> value v. v is kept only by
  // two hops, the second of which needs the first.
  void * p1 = nullptr;
  void * p2 = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kEph, &p1), GRHEAP_OK);
  ASSERT_EQ(grheap_alloc(w.heap, &kEph, &p2), GRHEAP_OK);
  auto * e1 = static_cast<Eph *>(p1);
  auto * e2 = static_cast<Eph *>(p2);
  Node * k1 = mk(w.heap, 1);
  Node * k2 = mk(w.heap, 2);
  Node * v = mk(w.heap, 3);
  ASSERT_EQ(grheap_store(w.heap, e1, &e1->key, k1), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, e1, &e1->value, k2), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, e2, &e2->key, k2), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, e2, &e2->value, v), GRHEAP_OK);
  void * r1 = k1;
  void * r2 = e1;
  void * r3 = e2;
  ASSERT_EQ(grheap_root_add(w.heap, &r1), GRHEAP_OK);
  ASSERT_EQ(grheap_root_add(w.heap, &r2), GRHEAP_OK);
  ASSERT_EQ(grheap_root_add(w.heap, &r3), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, v), GRHEAP_OK);
  ASSERT_EQ(path.count(), 3u);
  EXPECT_EQ(path.step(0).object, k1);
  EXPECT_EQ(path.step(0).ephemeron, e1);
  EXPECT_EQ(path.step(1).object, k2);
  EXPECT_EQ(path.step(1).ephemeron, e2);
  EXPECT_EQ(path.step(2).object, v);
  ASSERT_EQ(grheap_root_remove(w.heap, &r1), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &r2), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &r3), GRHEAP_OK);
}

/* ---- Garbage ------------------------------------------------------------------------ */

TEST(Retention, UnsweptGarbageIsASuccessfulAnswerOfNotRetainedNeverAnError) {
  HeapWorld w(steady());
  destroyed_log().clear();
  Node * garbage = mk(w.heap, 5, &kNode);
  Node * also = mk(w.heap, 6, &kNode);
  link_a(w, garbage, also);
  Path path;
  EXPECT_EQ(path.ask(w.heap, garbage), GRHEAP_OK);
  ASSERT_NE(path.p, nullptr);
  EXPECT_FALSE(path.retained());
  EXPECT_EQ(path.count(), 0u);
  GRHEAP_RetentionStep s;
  EXPECT_EQ(grheap_retention_step(path.p, 0, &s), GRHEAP_ERR_INVALID);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(destroyed(5));
  EXPECT_TRUE(destroyed(6));
}

/* ---- Refusals -------------------------------------------------------------------------- */

TEST(Retention, ARefusedQueryWritesNothingAndChangesNothing) {
  HeapWorld w(steady());
  Chain c = build_chain(w);
  void * root = c.a;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  std::vector<uint64_t> before = fingerprint(w.heap);
  GRHEAP_Retention * marker = reinterpret_cast<GRHEAP_Retention *>(&before);
  GRHEAP_Retention * out = marker;
  int on_stack = 0;
  const void * bad[] = {nullptr, reinterpret_cast<const char *>(c.b) + 8,
      reinterpret_cast<const char *>(c.b) + 1, &on_stack,
      reinterpret_cast<const char *>(c.b) + 16 + sizeof(Node)};
  for (const void * p : bad) {
    EXPECT_EQ(grheap_retention_path(w.heap, p, nullptr, &out), GRHEAP_ERR_INVALID);
    EXPECT_EQ(out, marker);
  }
  EXPECT_EQ(grheap_retention_path(nullptr, c.b, nullptr, &out), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_retention_path(w.heap, c.b, nullptr, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(fingerprint(w.heap), before);
  // The query itself is the control: a valid pointer works.
  Path path;
  EXPECT_EQ(path.ask(w.heap, c.b), GRHEAP_OK);
  EXPECT_TRUE(path.retained());
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST(Retention, AFreedObjectIsRefusedAsNotALivePayload) {
  HeapWorld w(steady());
  Node * doomed = mk(w.heap, 1);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK); // frees it
  GRHEAP_Retention * out = nullptr;
  EXPECT_EQ(grheap_retention_path(w.heap, doomed, nullptr, &out), GRHEAP_ERR_INVALID);
  EXPECT_EQ(out, nullptr);
}

TEST(Retention, ABusyHeapRefusesTheQueryFromInsideATraceAndADestroy) {
  HeapWorld w(steady());
  static GRHEAP_Heap * g_heap = nullptr;
  static GRHEAP_Result g_trace_result;
  static GRHEAP_Result g_destroy_result;
  static void * g_target;
  g_heap = w.heap;
  g_trace_result = g_destroy_result = GRHEAP_OK;
  struct Hooks {
    static void trace(GRHEAP_Tracer *, void *) {
      GRHEAP_Retention * out = nullptr;
      g_trace_result = grheap_retention_path(g_heap, g_target, nullptr, &out);
      grheap_retention_release(out);
    }
    static void destroy(void *) {
      GRHEAP_Retention * out = nullptr;
      g_destroy_result = grheap_retention_path(g_heap, g_target, nullptr, &out);
      grheap_retention_release(out);
    }
  };
  static const GRHEAP_Type kHooked = {"hooked", 16, Hooks::trace, Hooks::destroy,
      nullptr, nullptr, nullptr};
  void * hooked = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kHooked, &hooked), GRHEAP_OK);
  Node * holder = mk(w.heap, 1);
  Node * target = mk(w.heap, 2);
  link_a(w, holder, target);
  g_target = target;
  void * root = hooked;
  void * root2 = holder;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  ASSERT_EQ(grheap_root_add(w.heap, &root2), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK); // runs trace
  EXPECT_EQ(g_trace_result, GRHEAP_ERR_INVALID);
  // A query's own walk runs a `trace` with the heap busy too, which is why a
  // trace that asks is refused rather than recursing. (The hooked object is
  // expanded before the holder, since roots are discovered in order.)
  g_trace_result = GRHEAP_OK;
  Path path;
  ASSERT_EQ(path.ask(w.heap, target), GRHEAP_OK);
  EXPECT_EQ(g_trace_result, GRHEAP_ERR_INVALID);
  expect_chain(path, {holder, target}, {kA});
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &root2), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK); // runs destroy
  EXPECT_EQ(g_destroy_result, GRHEAP_ERR_INVALID);
}

TEST(Retention, ARunningContextIsRefusedAndAPausedOrParkedOneIsNot) {
  HeapWorld w(steady());
  Node * n = mk(w.heap, 1);
  void * root = n;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  GRHEAP_Result inside = GRHEAP_OK;
  Fn fn;
  fn.body = [&](GRCORE_Context *) {
    GRHEAP_Retention * out = nullptr;
    inside = grheap_retention_path(w.heap, n, nullptr, &out);
    EXPECT_EQ(out, nullptr);
    return GRCORE_STEP_FINISHED;
  };
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(inside, GRHEAP_ERR_INVALID);
  Path path;
  EXPECT_EQ(path.ask(w.heap, n), GRHEAP_OK); // parked again
  EXPECT_TRUE(path.retained());
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

namespace {
/* A service key whose OBSERVE handler asks the query, to show it works from a
 * poll handler (the context is at-poll). */
struct AtPoll {
  GRHEAP_Heap * heap;
  const void * object;
  GRHEAP_Result result = GRHEAP_ERR_INTERNAL;
  bool retained = false;
  size_t steps = 0;
};
void at_poll_handler(GRCORE_Context *, void * value, GRCORE_PollCall *) {
  auto * a = static_cast<AtPoll *>(value);
  GRHEAP_Retention * out = nullptr;
  a->result = grheap_retention_path(a->heap, a->object, nullptr, &out);
  if (a->result == GRHEAP_OK) {
    a->retained = grheap_retention_retained(out);
    a->steps = grheap_retention_step_count(out);
  }
  grheap_retention_release(out);
}
const GRCORE_Key kAtPollKey = {"at-poll-query", GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_OBSERVE, nullptr, at_poll_handler, nullptr, nullptr, nullptr};
} // namespace

TEST(Retention, AnObserveHandlerAtAPollCanAskAndItWorks) {
  HeapWorld w(steady());
  Chain c = build_chain(w);
  void * root = c.a;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  AtPoll at{w.heap, c.leaked};
  ASSERT_EQ(grcore_context_register(w.ctx, &kAtPollKey, &at), GRCORE_OK);
  GRCORE_RequestKind kind = 0;
  ASSERT_EQ(grcore_context_request_kind(w.ctx, &kAtPollKey, &kind), GRCORE_OK);
  GRCORE_Port * port = nullptr;
  ASSERT_EQ(grcore_context_port(w.ctx, &port), GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port, kind), GRCORE_OK);
  grcore_port_release(port);
  Fn fn;
  fn.body = [&](GRCORE_Context * ctx) {
    GRCORE_POLL(ctx);
    return GRCORE_STEP_FINISHED;
  };
  GRCORE_Outcome outcome;
  ASSERT_EQ(grcore_run(w.ctx, fn_entry, &fn, &outcome), GRCORE_OK);
  EXPECT_EQ(at.result, GRHEAP_OK);
  EXPECT_TRUE(at.retained);
  EXPECT_EQ(at.steps, 3u);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST(Retention, ANonOwnerThreadIsRefused) {
  HeapWorld w(steady());
  Node * n = mk(w.heap, 1);
  void * root = n;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  GRHEAP_Result r = GRHEAP_OK;
  GRHEAP_Retention * out = nullptr;
  std::thread([&] { r = grheap_retention_path(w.heap, n, nullptr, &out); }).join();
  EXPECT_EQ(r, GRHEAP_ERR_INVALID);
  EXPECT_EQ(out, nullptr);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

namespace {
GRCORE_Context * g_ctx_for_remove = nullptr;
const GRCORE_RootSource kVictim = {"victim", held_enumerate};
HeldRoots g_victim_roots;
void remover_enumerate(GRCORE_Context *, void *, const GRCORE_RootVisitor *) {
  // Breaks roots.h's rule (a source must not remove one) on purpose, so the
  // table is shorter than the count the query read.
  grcore_context_remove_root_source(g_ctx_for_remove, &kVictim, &g_victim_roots);
}
const GRCORE_RootSource kRemover = {"remover", remover_enumerate};

uint64_t g_outside_slot = 0;
void outside_trace(GRHEAP_Tracer * t, void *) { grheap_trace_word(t, &g_outside_slot); }
const GRHEAP_Type kOutside = {"outside", 16, outside_trace, nullptr, nullptr, nullptr, nullptr};
} // namespace

TEST(Retention, ARootSourceThatCannotBeReadFailsTheQueryInsteadOfTruncatingTheRoots) {
  HeapWorld w(steady());
  Node * unreached = mk(w.heap, 1);
  g_ctx_for_remove = w.ctx;
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kRemover, nullptr), GRCORE_OK);
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kVictim, &g_victim_roots), GRCORE_OK);
  std::vector<uint64_t> before = fingerprint(w.heap);
  GRHEAP_Retention * out = nullptr;
  EXPECT_EQ(grheap_retention_path(w.heap, unreached, nullptr, &out), GRHEAP_ERR_INVALID);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(fingerprint(w.heap), before);
}

TEST(Retention, ASlotOutsideThePayloadHasTheNotInlineOffset) {
  HeapWorld w(steady());
  void * po = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kOutside, &po), GRHEAP_OK);
  Node * target = mk(w.heap, 5);
  g_outside_slot = addr(target);
  void * root = po;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, target), GRHEAP_OK);
  ASSERT_EQ(path.count(), 2u);
  EXPECT_EQ(path.step(0).slot_offset, GRHEAP_RETENTION_NOT_INLINE);
  g_outside_slot = 0;
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST(Retention, AccessorsRefuseNullAndOutOfRange) {
  EXPECT_FALSE(grheap_retention_retained(nullptr));
  EXPECT_EQ(grheap_retention_step_count(nullptr), 0u);
  GRHEAP_RetentionRoot root;
  GRHEAP_RetentionStep step;
  EXPECT_EQ(grheap_retention_root(nullptr, &root), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_retention_step(nullptr, 0, &step), GRHEAP_ERR_INVALID);
  grheap_retention_release(nullptr);
  grheap_retention_dump(nullptr, stdout);
  HeapWorld w(steady());
  Node * n = mk(w.heap, 1);
  void * r = n;
  ASSERT_EQ(grheap_root_add(w.heap, &r), GRHEAP_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, n), GRHEAP_OK);
  EXPECT_EQ(grheap_retention_step(path.p, 1, &step), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_retention_step(path.p, 0, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_retention_root(path.p, nullptr), GRHEAP_ERR_INVALID);
  EXPECT_STREQ(grheap_retention_root_kind_string(GRHEAP_ROOT_PIN), "pin");
  EXPECT_STREQ(
      grheap_retention_root_kind_string(static_cast<GRHEAP_RootKind>(99)),
      "unknown");
  ASSERT_EQ(grheap_root_remove(w.heap, &r), GRHEAP_OK);
}

/* ---- Allocation failure ------------------------------------------------------------------ */

TEST(Retention, EveryScratchAllocationFailureIsACleanErrorThatChangesNothing) {
  HeapWorld w(steady());
  // A chain of 300: finding its end explores every node, so the node array and
  // the visited table each grow several times.
  const size_t N = 300;
  std::vector<Node *> chain;
  for (size_t i = 0; i < N; i++) {
    chain.push_back(mk(w.heap, i));
    if (i > 0) {
      link_a(w, chain[i - 1], chain[i]);
    }
  }
  void * root = chain[0];
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  std::vector<uint64_t> before = fingerprint(w.heap);
  long live_before = w.allocator.live;
  uint64_t in_use_before = grcore_context_memory_in_use(w.ctx);

  size_t failures = 0;
  bool succeeded = false;
  for (long n = 1; n <= 40 && !succeeded; n++) {
    w.allocator.calls = 0;
    w.allocator.fail_at = n;
    GRHEAP_Retention * out = nullptr;
    GRHEAP_Result r = grheap_retention_path(w.heap, chain[N - 1], nullptr, &out);
    w.allocator.fail_at = 0;
    if (r == GRHEAP_OK) {
      succeeded = true;
      ASSERT_NE(out, nullptr);
      EXPECT_EQ(grheap_retention_step_count(out), N);
      grheap_retention_release(out);
    } else {
      failures++;
      EXPECT_EQ(r, GRHEAP_ERR_OOM) << "n=" << n;
      EXPECT_EQ(out, nullptr);
    }
    EXPECT_EQ(w.allocator.live, live_before) << "n=" << n;
    EXPECT_EQ(grcore_context_memory_in_use(w.ctx), in_use_before) << "n=" << n;
    EXPECT_EQ(fingerprint(w.heap), before) << "n=" << n;
  }
  EXPECT_TRUE(succeeded);
  EXPECT_GE(failures, 4u); // nodes and table each grew more than once
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST(Retention, AllocationFailureOfTheResultWithTheCallersAllocatorIsCleanToo) {
  HeapWorld w(steady());
  Chain c = build_chain(w);
  void * root = c.a;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  TrackingAllocator mine;
  mine.fail_at = 1;
  GRHEAP_Retention * out = nullptr;
  std::vector<uint64_t> before = fingerprint(w.heap);
  EXPECT_EQ(grheap_retention_path(w.heap, c.leaked, mine.get(), &out), GRHEAP_ERR_OOM);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(mine.live, 0);
  EXPECT_EQ(w.allocator.live, w.allocator.live);
  EXPECT_EQ(fingerprint(w.heap), before);
  mine.fail_at = 0;
  Path path;
  ASSERT_EQ(path.ask(w.heap, c.leaked, mine.get()), GRHEAP_OK);
  EXPECT_EQ(mine.live, 1); // the result is one block of the caller's
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST(Retention, AMemoryBudgetThatRefusesScratchIsALimitAndChangesNothing) {
  WorldConfig config = steady();
  config.reserve = 0;
  HeapWorld w(config);
  Chain c = build_chain(w);
  void * root = c.a;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  ASSERT_EQ(grcore_context_set_memory_bytes(
                w.ctx, grcore_context_memory_in_use(w.ctx) + 16),
      GRCORE_OK);
  std::vector<uint64_t> before = fingerprint(w.heap);
  GRHEAP_Retention * out = nullptr;
  EXPECT_EQ(grheap_retention_path(w.heap, c.leaked, nullptr, &out), GRHEAP_ERR_LIMIT);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(fingerprint(w.heap), before);
  ASSERT_EQ(grcore_context_set_memory_bytes(w.ctx, GRCORE_UNLIMITED), GRCORE_OK);
  Path path;
  EXPECT_EQ(path.ask(w.heap, c.leaked), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

/* ---- Modes --------------------------------------------------------------------------------- */

TEST(Retention, UnderTortureAndBarrierVerifyTheQueryChangesNoStateAndNoStatistic) {
  WorldConfig config;
  config.torture = 1;
  config.verify = 1; // count
  HeapWorld w(config);
  // Every object is rooted the moment it is allocated, as torture requires.
  void * slots[3] = {nullptr, nullptr, nullptr};
  for (void *& s : slots) {
    ASSERT_EQ(grheap_root_add(w.heap, &s), GRHEAP_OK);
  }
  slots[0] = mk(w.heap, 1);
  slots[1] = mk(w.heap, 2);
  slots[2] = mk(w.heap, 3);
  auto * a = static_cast<Node *>(slots[0]);
  auto * b = static_cast<Node *>(slots[1]);
  auto * l = static_cast<Node *>(slots[2]);
  link_a(w, a, b);
  link_b(w, b, l);
  void * keep = a;
  ASSERT_EQ(grheap_root_add(w.heap, &keep), GRHEAP_OK);
  for (void *& s : slots) {
    ASSERT_EQ(grheap_root_remove(w.heap, &s), GRHEAP_OK);
  }
  std::vector<uint64_t> before = fingerprint(w.heap);
  GRHEAP_Stats stats_before = w.stats();
  for (int i = 0; i < 5; i++) {
    Path path;
    ASSERT_EQ(path.ask(w.heap, l), GRHEAP_OK);
    expect_chain(path, {a, b, l}, {kA, kB});
    EXPECT_EQ(fingerprint(w.heap), before) << "query " << i;
  }
  GRHEAP_Stats stats_after = w.stats();
  EXPECT_EQ(std::memcmp(&stats_before, &stats_after, sizeof stats_before), 0);
  EXPECT_EQ(stats_after.verify_violations, 0u);
  uint64_t violations = 1;
  ASSERT_EQ(grheap_verify(w.heap, &violations), GRHEAP_OK);
  EXPECT_EQ(violations, 0u);
  ASSERT_EQ(grheap_root_remove(w.heap, &keep), GRHEAP_OK);
}

TEST(Retention, AnIdIsNotIssuedByAskingAndIsStillUsableOnAStep) {
  HeapWorld w(steady());
  Chain c = build_chain(w);
  void * root = c.a;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  GRHEAP_Stats before = w.stats();
  Path path;
  ASSERT_EQ(path.ask(w.heap, c.leaked), GRHEAP_OK);
  GRHEAP_Stats after = w.stats();
  EXPECT_EQ(after.ids_issued, before.ids_issued);
  // A host that wants a durable name asks for it on the step it cares about.
  uint64_t id = 0;
  ASSERT_EQ(grheap_id_of(w.heap, path.step(2).object, &id), GRHEAP_OK);
  EXPECT_NE(id, 0u);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST(Retention, ItWorksInAnArenaHeapWhereRootsKeepNothingAliveButStillExplain) {
  WorldConfig config = steady();
  config.mode = GRHEAP_MODE_ARENA;
  HeapWorld w(config);
  Chain c = build_chain(w);
  Node * stray = mk(w.heap, 9);
  void * root = c.a;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  std::vector<uint64_t> before = fingerprint(w.heap);
  Path path;
  ASSERT_EQ(path.ask(w.heap, c.leaked), GRHEAP_OK);
  expect_chain(path, {c.a, c.b, c.leaked}, {kA, kB});
  Path none;
  ASSERT_EQ(none.ask(w.heap, stray), GRHEAP_OK);
  EXPECT_FALSE(none.retained());
  EXPECT_EQ(fingerprint(w.heap), before);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

namespace {
/* A root source that is a box of words. */
struct Box {
  std::vector<uint64_t> words;
};
void box_enumerate(
    GRCORE_Context *, void * value, const GRCORE_RootVisitor * visitor) {
  auto * box = static_cast<Box *>(value);
  if (visitor->slot != nullptr) {
    for (uint64_t & word : box->words) {
      visitor->slot(visitor->user, &word);
    }
  }
}
const GRCORE_RootSource kBoxSource = {"box", box_enumerate};

const GRHEAP_Type * resolve_plain(void *, const char * name) {
  return std::strcmp(name, "plain") == 0 ? &kPlainNode : nullptr;
}
} // namespace

TEST(Retention, ItWorksOverAHeapRestoredFromAnImage) {
  GRHEAP_Image * image = nullptr;
  {
    HeapWorld src(steady());
    Chain c = build_chain(src);
    Box box;
    box.words = {0, addr(c.a)};
    ASSERT_EQ(grcore_context_add_root_source(src.ctx, &kBoxSource, &box), GRCORE_OK);
    ASSERT_EQ(grheap_image_take(src.heap, nullptr, &image), GRHEAP_OK);
  }
  HeapWorld dst(steady());
  Box box;
  box.words = {0, 0};
  ASSERT_EQ(grcore_context_add_root_source(dst.ctx, &kBoxSource, &box), GRCORE_OK);
  GRHEAP_RestoreEnv env;
  env.resolve = resolve_plain;
  env.user = nullptr;
  ASSERT_EQ(grheap_image_instantiate(dst.heap, image, &env), GRHEAP_OK);
  grheap_image_release(image);
  auto * a = reinterpret_cast<Node *>(box.words[1]);
  ASSERT_NE(a, nullptr);
  auto * b = static_cast<Node *>(a->a);
  auto * l = static_cast<Node *>(b->b);
  std::vector<uint64_t> before = fingerprint(dst.heap);
  Path path;
  ASSERT_EQ(path.ask(dst.heap, l), GRHEAP_OK);
  EXPECT_STREQ(path.root().source, "box");
  EXPECT_EQ(path.root().ordinal, 1u);
  expect_chain(path, {a, b, l}, {kA, kB});
  EXPECT_EQ(fingerprint(dst.heap), before);
}

/* ---- The result ------------------------------------------------------------------------------ */

/* The dump goes to a FILE *. open_memstream is POSIX and Windows has none, so
 * the text goes through a temporary file, read back and removed. */
static std::string capture_dump(const GRHEAP_Retention * path) {
  std::string name = (std::filesystem::temp_directory_path() /
      ("grheap-retention-dump-" +
          std::to_string(std::chrono::steady_clock::now()
                             .time_since_epoch()
                             .count()) +
          ".txt"))
                         .string();
  FILE * f = fopen(name.c_str(), "wb");
  EXPECT_NE(f, nullptr);
  if (f == nullptr) {
    return std::string();
  }
  grheap_retention_dump(path, f);
  fclose(f);
  std::ifstream in(name, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)),
      std::istreambuf_iterator<char>());
  in.close();
  std::remove(name.c_str());
  return text;
}

TEST(Retention, TheDumpNamesTheRootAndEveryStepWithoutAnyAddress) {
  HeapWorld w(steady());
  Chain c = build_chain(w);
  uint64_t slot = addr(c.a);
  HeldRoots held;
  held.slots = {&slot};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held), GRCORE_OK);
  Path path;
  ASSERT_EQ(path.ask(w.heap, c.leaked), GRHEAP_OK);
  std::string s = capture_dump(path.p);
  EXPECT_NE(s.find("root: context root source \"held\", #0"), std::string::npos) << s;
  EXPECT_NE(s.find("step 0: object type \"plain\", slot offset 0"), std::string::npos) << s;
  EXPECT_NE(s.find("step 1: object type \"plain\", slot offset 8"), std::string::npos) << s;
  EXPECT_NE(s.find("step 2: object type \"plain\" (the target)"), std::string::npos) << s;
  EXPECT_EQ(s.find("0x"), std::string::npos) << s;
}

TEST(Retention, TheResultOutlivesTheWorldItCameFrom) {
  GRHEAP_Retention * path = nullptr;
  {
    HeapWorld w(steady());
    Chain c = build_chain(w);
    void * root = c.a;
    ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
    ASSERT_EQ(grheap_retention_path(w.heap, c.leaked, nullptr, &path), GRHEAP_OK);
    ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
  }
  // The heap is gone; the counts and types (static) are still readable, and
  // releasing it needs nothing of the context.
  EXPECT_TRUE(grheap_retention_retained(path));
  EXPECT_EQ(grheap_retention_step_count(path), 3u);
  GRHEAP_RetentionStep s;
  ASSERT_EQ(grheap_retention_step(path, 2, &s), GRHEAP_OK);
  EXPECT_STREQ(s.type->name, "plain");
  grheap_retention_release(path);
}

/* ---- A randomised graph against an independent search ---------------------------------------- */

TEST(Retention, RandomGraphsAgreeWithAnIndependentBreadthFirstSearch) {
  std::mt19937_64 rng(20261004);
  for (int round = 0; round < 150; round++) {
    HeapWorld w(steady());
    size_t n = 4 + rng() % 40;
    std::vector<Node *> nodes;
    for (size_t i = 0; i < n; i++) {
      nodes.push_back(mk(w.heap, i));
    }
    // Edges: each slot points at a random node or at nothing.
    std::vector<std::array<int, 2>> edge(n, {-1, -1});
    for (size_t i = 0; i < n; i++) {
      for (int s = 0; s < 2; s++) {
        if (rng() % 3 != 0) {
          edge[i][s] = static_cast<int>(rng() % n);
          if (s == 0) {
            link_a(w, nodes[i], nodes[edge[i][s]]);
          } else {
            link_b(w, nodes[i], nodes[edge[i][s]]);
          }
        }
      }
    }
    // Roots: some C roots (in order), then some handles (newest first).
    size_t nc = 1 + rng() % 3;
    size_t nh = rng() % 3;
    std::vector<int> c_roots, h_roots;
    std::vector<void *> slots(nc);
    for (size_t i = 0; i < nc; i++) {
      c_roots.push_back(static_cast<int>(rng() % n));
      slots[i] = nodes[c_roots.back()];
      ASSERT_EQ(grheap_root_add(w.heap, &slots[i]), GRHEAP_OK);
    }
    std::vector<GRHEAP_Handle *> handles;
    for (size_t i = 0; i < nh; i++) {
      h_roots.push_back(static_cast<int>(rng() % n));
      GRHEAP_Handle * h = nullptr;
      ASSERT_EQ(grheap_handle_create(w.heap, nodes[h_roots.back()], &h), GRHEAP_OK);
      handles.push_back(h);
    }
    // The independent search: roots in the collector's order, edges in slot
    // order, first discovery wins.
    std::vector<int> order = c_roots;
    for (size_t i = nh; i-- > 0;) {
      order.push_back(h_roots[i]);
    }
    std::vector<int> parent(n, -2), slot_of(n, 0);
    std::vector<size_t> root_pos(n, 0);
    std::deque<int> queue;
    for (size_t pos = 0; pos < order.size(); pos++) {
      int r = order[pos];
      if (parent[r] == -2) {
        parent[r] = -1;
        root_pos[r] = pos;
        queue.push_back(r);
      }
    }
    while (!queue.empty()) {
      int x = queue.front();
      queue.pop_front();
      for (int s = 0; s < 2; s++) {
        int y = edge[x][s];
        if (y >= 0 && parent[y] == -2) {
          parent[y] = x;
          slot_of[y] = s;
          queue.push_back(y);
        }
      }
    }
    std::vector<uint64_t> before = fingerprint(w.heap);
    for (size_t t = 0; t < n; t++) {
      Path path;
      ASSERT_EQ(path.ask(w.heap, nodes[t]), GRHEAP_OK);
      if (parent[t] == -2) {
        EXPECT_FALSE(path.retained()) << "round " << round << " node " << t;
        continue;
      }
      std::vector<int> chain;
      for (int x = static_cast<int>(t); x != -1; x = parent[x]) {
        chain.push_back(x);
      }
      std::reverse(chain.begin(), chain.end());
      ASSERT_TRUE(path.retained()) << "round " << round << " node " << t;
      ASSERT_EQ(path.count(), chain.size()) << "round " << round << " node " << t;
      for (size_t i = 0; i < chain.size(); i++) {
        GRHEAP_RetentionStep s = path.step(i);
        EXPECT_EQ(s.object, nodes[chain[i]]);
        if (i + 1 < chain.size()) {
          EXPECT_EQ(s.slot_offset, slot_of[chain[i + 1]] == 0 ? kA : kB);
        }
      }
      // The root's identity: a C root's index, or a handle's position.
      GRHEAP_RetentionRoot root = path.root();
      size_t pos = root_pos[chain[0]];
      if (pos < nc) {
        EXPECT_EQ(root.kind, GRHEAP_ROOT_C_ROOT);
        EXPECT_EQ(root.ordinal, pos);
      } else {
        EXPECT_EQ(root.kind, GRHEAP_ROOT_HANDLE);
        EXPECT_EQ(root.ordinal, pos - nc);
      }
    }
    EXPECT_EQ(fingerprint(w.heap), before);
    for (GRHEAP_Handle * h : handles) {
      grheap_handle_destroy(h);
    }
    for (size_t i = 0; i < nc; i++) {
      grheap_root_remove(w.heap, &slots[i]);
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
