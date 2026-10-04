/**
 * @file
 *
 * Marking: the value codec, precise slots against conservative ranges,
 * interior and one-past-the-end pointers, and a mark stack that cannot grow.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstring>

namespace {

/* A Payload holding one encoded word, traced with grheap_trace_word. */
struct Boxed {
  uint64_t word;
};

void boxed_trace(GRHEAP_Tracer * t, void * payload) {
  grheap_trace_word(t, &static_cast<Boxed *>(payload)->word);
}

const GRHEAP_Type kBoxed = {"boxed", sizeof(Boxed), boxed_trace, nullptr, nullptr, nullptr, nullptr};

/* A payload holding one raw slot, whatever it contains. */
void slotted_trace(GRHEAP_Tracer * t, void * payload) {
  grheap_trace_slot(t, static_cast<void **>(payload));
}

const GRHEAP_Type kSlotted = {"slotted", 16, slotted_trace, nullptr, nullptr, nullptr, nullptr};

void add_source(HeapWorld & w, HeldRoots & held) {
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held),
      GRCORE_OK);
}

} // namespace

class MarkTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

/* ---- The value codec --------------------------------------------------- */

TEST_F(MarkTest, ATaggedWordMarksItsObjectAndAnUntaggedOneIsNotAPointer) {
  WorldConfig config;
  config.codec_set = true;
  // A pointer p is the word (p << 1) | 1; a clear tag bit is an integer.
  config.codec = GRHEAP_ValueCodec{1, 1, ~UINT64_C(1), 1, 0};
  HeapWorld w(steady(config));
  Node * tagged = new_node(w.heap, 1);
  Node * plain = new_node(w.heap, 2);
  uint64_t tagged_word = (addr(tagged) << 1) | 1u;
  uint64_t integer_word = addr(plain) << 1; // looks like it, tag bit clear
  HeldRoots held;
  held.slots = {&tagged_word, &integer_word};
  add_source(w, held);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, tagged));
  EXPECT_FALSE(grheap_contains(w.heap, plain));
  // A word that fails the tag test is simply not a pointer; it is not an
  // invalid one.
  EXPECT_EQ(w.stats().invalid_pointers, 0u);
}

TEST_F(MarkTest, TheCodecsMaskAndShiftDecodeAWordToAnAddress) {
  // A word is the address shifted up by two with junk above the mask; the
  // mask strips the junk and the shift brings the address back.
  WorldConfig config;
  config.codec_set = true;
  config.codec = GRHEAP_ValueCodec{0, 0, UINT64_C(0x00FFFFFFFFFFFFFF), 2, 0};
  HeapWorld w(steady(config));
  Node * n = new_node(w.heap, 1);
  Node * other = new_node(w.heap, 2);
  uint64_t word = (addr(n) << 2) | (UINT64_C(0xAB) << 56);
  uint64_t wrong = addr(other); // decodes to a quarter of the address
  HeldRoots held;
  held.slots = {&word, &wrong};
  add_source(w, held);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, n));
  EXPECT_FALSE(grheap_contains(w.heap, other));
  // No tag test is set, so the second word is a pointer, just not to anything.
  EXPECT_EQ(w.stats().invalid_pointers, 1u);
}

TEST_F(MarkTest, ABaseIsAddedAfterTheShift) {
  // A word is an offset from a base, scaled by 16: address = (word >> 4) + base.
  // The base is a constant, so the test does not depend on where the provider
  // happens to map the first block.
  constexpr uint64_t kBase = 0x1000;
  WorldConfig config;
  config.codec_set = true;
  config.codec = GRHEAP_ValueCodec{0, 0, UINT64_MAX, 4, kBase};
  HeapWorld w(steady(config));
  Node * n = new_node(w.heap, 1);
  Node * other = new_node(w.heap, 2);
  uint64_t offset = (addr(n) - kBase) << 4;
  uint64_t undecoded = addr(other) << 4; // forgets the base: lands 0x1000 off
  HeldRoots held;
  held.slots = {&offset, &undecoded};
  add_source(w, held);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, n));
  EXPECT_FALSE(grheap_contains(w.heap, other));
  EXPECT_EQ(w.stats().invalid_pointers, 1u);
}

TEST_F(MarkTest, TraceWordDecodesWithTheHeapsCodecToo) {
  WorldConfig config;
  config.codec_set = true;
  config.codec = GRHEAP_ValueCodec{1, 1, ~UINT64_C(1), 1, 0};
  HeapWorld w(steady(config));
  void * outer_p;
  ASSERT_EQ(grheap_alloc(w.heap, &kBoxed, &outer_p), GRHEAP_OK);
  auto * outer = static_cast<Boxed *>(outer_p);
  Node * inner = new_node(w.heap, 1);
  Node * stray = new_node(w.heap, 2);
  ASSERT_EQ(grheap_store_word(w.heap, outer, &outer->word, (addr(inner) << 1) | 1u),
      GRHEAP_OK);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, outer, &h), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, inner));
  EXPECT_FALSE(grheap_contains(w.heap, stray));
  // An integer in the slot is ignored, and frees what it used to hold.
  ASSERT_EQ(grheap_store_word(w.heap, outer, &outer->word, 42u << 1), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, inner));
  EXPECT_EQ(w.stats().invalid_pointers, 0u);
}

TEST_F(MarkTest, WithTheIdentityCodecEveryWordIsAPointerAndAnIntegerIsInvalid) {
  HeapWorld w;
  uint64_t integer = 12345;
  HeldRoots held;
  held.slots = {&integer};
  add_source(w, held);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().invalid_pointers, 1u);
  // An address of zero is nothing at all, not a bad pointer.
  uint64_t zero = 0;
  held.slots = {&zero};
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().invalid_pointers, 1u);
}

/* ---- Precise slots against conservative ranges ------------------------ */

TEST_F(MarkTest, APreciseSlotMustBeAPayloadStartOrItIsSkippedAndCounted) {
  HeapWorld w(steady());
  Node * interior = new_node(w.heap, 1);
  Node * past_end = new_node(w.heap, 2);
  Node * exact = new_node(w.heap, 3);
  uint64_t s_interior = addr(interior) + 8;
  uint64_t s_past = addr(past_end) + sizeof(Node);
  uint64_t s_exact = addr(exact);
  HeldRoots held;
  held.slots = {&s_interior, &s_past, &s_exact};
  add_source(w, held);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, exact));
  EXPECT_FALSE(grheap_contains(w.heap, interior));
  EXPECT_FALSE(grheap_contains(w.heap, past_end));
  EXPECT_EQ(w.stats().invalid_pointers, 2u);
  EXPECT_EQ(w.stats().conservative_hits, 0u);
}

TEST_F(MarkTest, AConservativeRangeAcceptsInteriorAndOnePastEndPointers) {
  HeapWorld w(steady());
  Node * obj = new_node(w.heap, 1);
  Node * unrelated = new_node(w.heap, 2);
  uint64_t words[] = {addr(obj) + 8,                // interior
      addr(obj) + sizeof(Node),                     // one past the end
      7,                                            // an integer
      addr(unrelated) ^ (UINT64_C(1) << 40),        // nowhere
      addr(&words) /* the array itself: not heap */};
  HeldRoots held;
  held.ranges = {range_of(words, 5)};
  add_source(w, held);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, obj));
  EXPECT_FALSE(grheap_contains(w.heap, unrelated));
  EXPECT_EQ(w.stats().conservative_hits, 2u);
  EXPECT_EQ(w.stats().invalid_pointers, 0u); // a range never calls anything invalid
  // The hit holds for one cycle only: without the range the object dies.
  held.ranges.clear();
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, obj));
}

TEST_F(MarkTest, TheSameAddressIsSkippedPreciselyAndAcceptedConservatively) {
  HeapWorld w;
  Node * obj = new_node(w.heap, 1);
  uint64_t slot = addr(obj) + 8;
  HeldRoots precise;
  precise.slots = {&slot};
  add_source(w, precise);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, obj));
  EXPECT_EQ(w.stats().invalid_pointers, 1u);

  Node * obj2 = new_node(w.heap, 2);
  uint64_t word = addr(obj2) + 8;
  precise.slots.clear();
  precise.ranges = {range_of(&word, 1)};
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, obj2));
  EXPECT_EQ(w.stats().conservative_hits, 1u);
}

TEST_F(MarkTest, AConservativeWordInAnObjectsHeaderOrPadIsStillInsideItsCell) {
  HeapWorld w;
  Node * obj = new_node(w.heap, 1);
  uint64_t words[] = {addr(obj) - 16, addr(obj) - 1, addr(obj) + sizeof(Node)};
  for (uint64_t word : words) {
    HeldRoots held;
    uint64_t w1 = word;
    held.ranges = {range_of(&w1, 1)};
    ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held),
        GRCORE_OK);
    uint64_t before = w.stats().conservative_hits;
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    EXPECT_EQ(w.stats().conservative_hits, before + 1) << std::hex << word;
    EXPECT_TRUE(grheap_contains(w.heap, obj)) << std::hex << word;
    ASSERT_EQ(grcore_context_remove_root_source(w.ctx, &kHeldSource, &held),
        GRCORE_OK);
  }
}

TEST_F(MarkTest, APointerOneAfterThePayloadNeverLandsOnTheNextObject) {
  // AD-11: objects are padded so a one-past-the-end pointer stays inside its
  // own object. Lay down a run of equal objects, point one past the end of
  // one, and require that that object alone survives.
  HeapWorld w(steady());
  const size_t sizes[] = {0, 1, 15, 16, 17, 31, 32, 100, 1000, 4000};
  for (size_t size : sizes) {
    for (size_t pick : {size_t(0), size_t(11), size_t(23)}) {
      std::vector<void *> objs;
      for (int i = 0; i < 24; i++) {
        void * p;
        ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, size, &p), GRHEAP_OK);
        objs.push_back(p);
      }
      uint64_t word = addr(objs[pick]) + size;
      HeldRoots held;
      held.ranges = {range_of(&word, 1)};
      ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held),
          GRCORE_OK);
      ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
      ASSERT_EQ(grcore_context_remove_root_source(w.ctx, &kHeldSource, &held),
          GRCORE_OK);
      for (size_t i = 0; i < objs.size(); i++) {
        EXPECT_EQ(grheap_contains(w.heap, objs[i]), i == pick)
            << "size " << size << " pick " << pick << " object " << i;
      }
      ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK); // and now none
      EXPECT_EQ(w.stats().live_objects, 0u);
    }
  }
}

TEST_F(MarkTest, AConservativeRangeUsesItsOwnMaskShiftAndBase) {
  HeapWorld w(steady());
  Node * obj = new_node(w.heap, 1);
  Node * other = new_node(w.heap, 2);
  uint64_t words[] = {(addr(obj) << 3) | 5u, addr(other)};
  GRCORE_ConservativeRange r = range_of(words, 2);
  r.mask = ~UINT64_C(7);
  r.shift = 3;
  HeldRoots held;
  held.ranges = {r};
  add_source(w, held);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, obj));
  // The second word decodes to an eighth of an address: nothing.
  EXPECT_FALSE(grheap_contains(w.heap, other));
  EXPECT_EQ(w.stats().conservative_hits, 1u);
}

TEST_F(MarkTest, ARangeThatIsEmptyUnalignedOrHasABadShiftIsHarmless) {
  HeapWorld w;
  Node * obj = new_node(w.heap, 1);
  alignas(8) uint64_t words[3] = {0, addr(obj), 0};
  GRCORE_ConservativeRange empty = range_of(words, 0);
  // Starts one byte into the second word and ends in the third: the only
  // whole aligned word inside is the third, which is zero.
  GRCORE_ConservativeRange skewed = {addr(&words[1]) + 1, addr(&words[2]) + 8,
      UINT64_MAX, 0, 0};
  GRCORE_ConservativeRange backwards = {addr(&words[2]), addr(&words[0]),
      UINT64_MAX, 0, 0};
  GRCORE_ConservativeRange bad_shift = range_of(words, 3);
  bad_shift.shift = 64;
  HeldRoots held;
  held.ranges = {empty, skewed, backwards, bad_shift};
  add_source(w, held);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(grheap_contains(w.heap, obj));
  EXPECT_EQ(w.stats().conservative_hits, 0u);
}

TEST_F(MarkTest, AConservativeWordAtAFreedCellOrAnUnusedCellIsNotAHit) {
  WorldConfig config;
  config.auto_collect = false;
  HeapWorld w(steady(config));
  Node * keep = new_node(w.heap, 1);
  Node * freed = new_node(w.heap, 2);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, keep, &h), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK); // frees `freed`
  uint64_t words[] = {addr(freed) + 8, addr(freed), addr(keep) + 4096 * 8};
  HeldRoots held;
  held.ranges = {range_of(words, 3)};
  add_source(w, held);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().conservative_hits, 0u);
  EXPECT_FALSE(grheap_contains(w.heap, freed));
}

TEST_F(MarkTest, ARangeOverARealStackBufferThatHoldsAPointerKeepsItsObject) {
  HeapWorld w;
  Node * obj = new_node(w.heap, 1);
  Node * obj2 = new_node(w.heap, 2);
  // An array of stale-looking junk with two real pointers in it, as a C stack
  // frame would have.
  volatile uint64_t frame[64];
  for (int i = 0; i < 64; i++) {
    frame[i] = 0xDEADBEEF00000000ull + static_cast<uint64_t>(i);
  }
  frame[10] = addr(obj);
  frame[40] = addr(obj2) + 8;
  HeldRoots held;
  held.ranges = {range_of(const_cast<const uint64_t *>(frame), 64)};
  add_source(w, held);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_TRUE(grheap_contains(w.heap, obj));
  EXPECT_TRUE(grheap_contains(w.heap, obj2));
  EXPECT_EQ(w.stats().conservative_hits, 2u);
}

TEST_F(MarkTest, TraceSlotAcceptsNullAndCountsAnythingElseThatIsNotAPayloadStart) {
  HeapWorld w(steady());
  void * holder_p;
  ASSERT_EQ(grheap_alloc(w.heap, &kSlotted, &holder_p), GRHEAP_OK);
  void ** slot = static_cast<void **>(holder_p);
  GRHEAP_Handle * h;
  ASSERT_EQ(grheap_handle_create(w.heap, holder_p, &h), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().invalid_pointers, 0u); // NULL is allowed

  int local = 0;
  ASSERT_EQ(grheap_store(w.heap, holder_p, slot, &local), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().invalid_pointers, 1u);

  Node * n = new_node(w.heap, 1);
  ASSERT_EQ(grheap_store(w.heap, holder_p, slot, reinterpret_cast<char *>(n) + 8),
      GRHEAP_OK); // interior
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().invalid_pointers, 2u);
  EXPECT_FALSE(grheap_contains(w.heap, n));

  Node * m = new_node(w.heap, 2);
  ASSERT_EQ(grheap_store(w.heap, holder_p, slot, m), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().invalid_pointers, 2u);
  EXPECT_TRUE(grheap_contains(w.heap, m));

  // A trace call with nothing to report is ignored.
  grheap_trace_slot(nullptr, slot);
  grheap_trace_word(nullptr, nullptr);
}

/* ---- A mark stack that cannot grow ------------------------------------- */

TEST_F(MarkTest, WhenTheMarkStackCannotGrowTheOverflowRescanStillMarksEverything) {
  HeapWorld w(steady());
  // A tree with sharing and a back edge, 1500 nodes, plus unreachable nodes
  // that point into it.
  constexpr size_t kTree = 1500;
  constexpr size_t kGarbage = 300;
  std::vector<Node *> tree;
  for (size_t i = 0; i < kTree; i++) {
    tree.push_back(new_node(w.heap, i));
  }
  for (size_t i = 0; i < kTree; i++) {
    if (2 * i + 1 < kTree) {
      ASSERT_EQ(grheap_store(w.heap, tree[i], &tree[i]->a, tree[2 * i + 1]),
          GRHEAP_OK);
    }
    if (2 * i + 2 < kTree) {
      ASSERT_EQ(grheap_store(w.heap, tree[i], &tree[i]->b, tree[2 * i + 2]),
          GRHEAP_OK);
    }
  }
  ASSERT_EQ(grheap_store(w.heap, tree[kTree - 1], &tree[kTree - 1]->a, tree[0]),
      GRHEAP_OK); // a cycle back to the root
  void * root = tree[0];
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  for (size_t i = 0; i < kGarbage; i++) {
    Node * g = new_node(w.heap, 100000 + i);
    ASSERT_EQ(grheap_store(w.heap, g, &g->a, tree[i % kTree]), GRHEAP_OK);
    ASSERT_EQ(grheap_store(w.heap, g, &g->b, g), GRHEAP_OK); // and a cycle of its own
  }
  long calls_before = w.allocator.calls;
  w.allocator.fail_all = true;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  w.allocator.fail_all = false;
  // The mark stack did try to grow, and failed, every time.
  EXPECT_GT(w.allocator.calls, calls_before);
  for (size_t i = 0; i < kTree; i++) {
    ASSERT_TRUE(grheap_contains(w.heap, tree[i])) << "tree node " << i;
    ASSERT_EQ(destroyed_count(i), 0u) << "tree node " << i;
  }
  for (size_t i = 0; i < kGarbage; i++) {
    EXPECT_EQ(destroyed_count(100000 + i), 1u) << "garbage " << i;
  }
  EXPECT_EQ(w.stats().live_objects, kTree);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST_F(MarkTest, ALongChainFoundBackwardsNeedsOnePassPerLinkAndStillCompletes) {
  // Each new node points to the one before it, so the chain is found from the
  // newest link back to the oldest: with the stack unable to hold a single
  // entry, every pass of the rescan finds the next link and no more.
  WorldConfig config;
  config.auto_collect = false;
  HeapWorld w(config);
  constexpr size_t kLength = 1500;
  void * head = nullptr;
  ASSERT_EQ(grheap_root_add(w.heap, &head), GRHEAP_OK);
  for (size_t i = 0; i < kLength; i++) {
    Node * n = new_node(w.heap, i);
    ASSERT_EQ(grheap_store(w.heap, n, &n->a, head), GRHEAP_OK);
    head = n;
  }
  new_node(w.heap, 999999); // garbage
  w.allocator.fail_all = true;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  w.allocator.fail_all = false;
  EXPECT_EQ(w.stats().live_objects, kLength);
  EXPECT_TRUE(destroyed(999999));
  EXPECT_EQ(destroyed_log().size(), 1u);
  ASSERT_EQ(grheap_root_remove(w.heap, &head), GRHEAP_OK);
}

TEST_F(MarkTest, ACollectionNeverAllocatesFromTheHeapOnItsOwn) {
  WorldConfig config;
  config.auto_collect = false;
  HeapWorld w(config);
  for (int i = 0; i < 100; i++) {
    new_node(w.heap, 0, &kPlainNode);
  }
  GRHEAP_Stats before = w.stats();
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().allocations, before.allocations);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
