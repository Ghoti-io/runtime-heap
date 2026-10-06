/**
 * @file
 *
 * Relocation torture (relocate.h): a collection moves every unpinned object,
 * rewrites every reference the collector was shown, and poisons the old cell.
 * Built only by `make RELOCATE=yes`.
 *
 * Objects are held across a collection only by a root slot, a handle, a pin or
 * a root source, because that is what a moving collector can update; a test
 * that keeps an address in a C variable is doing it to show that the address
 * goes stale. Poison is read from a cell the test keeps mapped (a pinned
 * neighbour of its size class), so ASan has nothing to say about it.
 *
 * Each planted library defect named in the Makefile (GRHEAP_PLANT_RELOC_*) is
 * meant to make one of these tests fail; `make check-relocation-gates` builds
 * the library with each in turn and requires it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

#ifdef _WIN32
static int setenv(const char * n, const char * v, int) {
  return _putenv_s(n, v);
}
static int unsetenv(const char * n) {
  return _putenv_s(n, "");
}
#endif

namespace {

constexpr uint64_t kPoison = UINT64_C(0xDDDDDDDDDDDDDDDD);

WorldConfig moving() {
  WorldConfig c;
  c.torture = 0;
  c.verify = 0;
  c.relocate = 1;
  c.auto_collect = false;
  return c;
}

GRHEAP_RelocationStats reloc_stats(const HeapWorld & w) {
  GRHEAP_RelocationStats s = {};
  EXPECT_EQ(grheap_relocation_stats(w.heap, &s), GRHEAP_OK);
  return s;
}

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

/* A keeper in the size class of `kPlainNode` and `kLeaf`'s cells (both 32
 * bytes), pinned so that it never moves and its block stays mapped, so a read of
 * a stale address inside that block is a read of poison and not a fault. */
void * pinned_keeper(HeapWorld & w) {
  Node * keeper = new_node(w.heap, 999, &kPlainNode);
  EXPECT_EQ(grheap_pin(w.heap, keeper), GRHEAP_OK);
  return keeper;
}

uint64_t read_word(const void * p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

/* A type with raw slots and a word slot. */
struct Mixed {
  void * ptr;
  uint64_t word; // a codec word
  uint64_t plain; // not traced: a plain integer
};
void mixed_trace(GRHEAP_Tracer * t, void * payload) {
  auto * m = static_cast<Mixed *>(payload);
  grheap_trace_slot(t, &m->ptr);
  grheap_trace_word(t, &m->word);
}
const GRHEAP_Type kMixed = GRHEAP_TYPE_INIT("mixed", sizeof(Mixed), mixed_trace, nullptr, nullptr, nullptr, nullptr);

/* A node whose first slot is reported through a copy: the collector sees the
 * target and keeps it, and the slot itself is never written. */
void copy_trace(GRHEAP_Tracer * t, void * payload) {
  auto * n = static_cast<Node *>(payload);
  void * copy = n->a;
  grheap_trace_slot(t, &copy);
}
const GRHEAP_Type kCopyReporter = GRHEAP_TYPE_INIT("copy-reporter", sizeof(Node), copy_trace, nullptr, nullptr, nullptr, nullptr);

/* A node whose trace leaves out the slot `b`. */
void omit_trace(GRHEAP_Tracer * t, void * payload) {
  grheap_trace_slot(t, &static_cast<Node *>(payload)->a);
}
const GRHEAP_Type kOmitter = GRHEAP_TYPE_INIT("omitter", sizeof(Node), omit_trace, nullptr, nullptr, nullptr, nullptr);

/* A root source that reports each of its words through a copy. */
struct CopyRoots {
  std::vector<uint64_t> words;
};
void copy_enumerate(GRCORE_Context *, void * value, const GRCORE_RootVisitor * visitor) {
  auto * c = static_cast<CopyRoots *>(value);
  for (uint64_t w : c->words) {
    uint64_t copy = w;
    visitor->slot(visitor->user, &copy);
  }
}
const GRCORE_RootSource kCopySource = GRCORE_ROOT_SOURCE_INIT("copy", copy_enumerate);

} // namespace

class RelocateTest : public ::testing::Test {
 protected:
  void SetUp() override { destroyed_log().clear(); }
};

/* ---- Rooted objects ---------------------------------------------------- */

TEST_F(RelocateTest, AnObjectHeldByARootSlotAHandleAndARootSourceMovesAndAllThreeFollowIt) {
  HeapWorld w(moving());
  void * keeper = pinned_keeper(w);
  (void)keeper;
  Node * a = new_node(w.heap, 11, &kPlainNode);
  Node * b = new_node(w.heap, 22, &kPlainNode);
  Node * c = new_node(w.heap, 33, &kPlainNode);
  void * slot = a;
  GRHEAP_Handle * handle = nullptr;
  ASSERT_EQ(grheap_handle_create(w.heap, b, &handle), GRHEAP_OK);
  ASSERT_EQ(grheap_root_add(w.heap, &slot), GRHEAP_OK);
  uint64_t source_word = addr(c);
  HeldRoots held;
  held.slots = {&source_word};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held), GRCORE_OK);
  const uint64_t old_a = addr(a), old_b = addr(b), old_c = addr(c);

  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);

  EXPECT_NE(addr(slot), old_a);
  EXPECT_NE(addr(grheap_handle_get(handle)), old_b);
  EXPECT_NE(source_word, old_c);
  EXPECT_TRUE(grheap_contains(w.heap, slot));
  EXPECT_TRUE(grheap_contains(w.heap, grheap_handle_get(handle)));
  EXPECT_TRUE(grheap_contains(w.heap, reinterpret_cast<void *>(source_word)));
  EXPECT_FALSE(grheap_contains(w.heap, reinterpret_cast<void *>(old_a)));
  EXPECT_EQ(static_cast<Node *>(slot)->id, 11u);
  EXPECT_EQ(static_cast<Node *>(grheap_handle_get(handle))->id, 22u);
  EXPECT_EQ(reinterpret_cast<Node *>(source_word)->id, 33u);
  EXPECT_EQ(reloc_stats(w).moved, 3u);
  EXPECT_EQ(reloc_stats(w).stayed_pinned, 1u);
  // Moved again at the next collection, and again.
  uint64_t prev = addr(slot);
  for (int i = 0; i < 5; i++) {
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    EXPECT_NE(addr(slot), prev) << "collection " << i;
    prev = addr(slot);
    EXPECT_EQ(static_cast<Node *>(slot)->id, 11u);
  }
  ASSERT_EQ(grheap_handle_destroy(handle), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &slot), GRHEAP_OK);
}

TEST_F(RelocateTest, ACodecEncodedWordIsRewrittenKeepingItsTagAndItsOtherBits) {
  WorldConfig c = moving();
  c.codec_set = true;
  // A pointer p is the word (p << 1) | 1, with a spare high bit carried along.
  c.codec = GRHEAP_ValueCodec{1, 1, ~UINT64_C(1) >> 1, 1, 0};
  HeapWorld w(c);
  void * keeper = pinned_keeper(w);
  (void)keeper;
  Node * n = new_node(w.heap, 5, &kPlainNode);
  const uint64_t spare = UINT64_C(1) << 63;
  uint64_t held_word = ((addr(n) << 1) | 1u) | spare;
  uint64_t integer = 0x40; // tag bit clear: an integer, not a reference
  HeldRoots held;
  held.slots = {&held_word, &integer};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held), GRCORE_OK);
  void * mp = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kMixed, &mp), GRHEAP_OK);
  auto * mixed = static_cast<Mixed *>(mp);
  mixed->ptr = n;
  mixed->word = (addr(n) << 1) | 1u;
  mixed->plain = addr(n); // untraced and looks like a pointer: must not be touched
  void * root = mp;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  const uint64_t old_n = addr(n);

  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);

  EXPECT_EQ(held_word & 1u, 1u);
  EXPECT_EQ(held_word & spare, spare) << "the bits outside the address survive";
  Node * now = reinterpret_cast<Node *>((held_word & ~spare) >> 1);
  EXPECT_NE(addr(now), old_n);
  EXPECT_TRUE(grheap_contains(w.heap, now));
  EXPECT_EQ(now->id, 5u);
  EXPECT_EQ(integer, 0x40u);
  auto * moved_mixed = static_cast<Mixed *>(root);
  EXPECT_EQ(moved_mixed->ptr, now);
  EXPECT_EQ(moved_mixed->word, (addr(now) << 1) | 1u);
  EXPECT_EQ(moved_mixed->plain, old_n) << "an untraced word is never rewritten";
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

/* ---- A graph ------------------------------------------------------------ */

TEST_F(RelocateTest, AGraphWithACycleAndASelfReferenceReadsTheSameAtNewAddresses) {
  HeapWorld w(moving());
  void * keeper = pinned_keeper(w);
  (void)keeper;
  Node * a = new_node(w.heap, 1, &kPlainNode);
  Node * b = new_node(w.heap, 2, &kPlainNode);
  Node * c = new_node(w.heap, 3, &kPlainNode);
  Node * d = new_node(w.heap, 4, &kPlainNode);
  void * root = a;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, a, &a->a, b), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, a, &a->b, d), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, b, &b->a, c), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, c, &c->a, a), GRHEAP_OK); // the cycle a, b, c
  ASSERT_EQ(grheap_store(w.heap, c, &c->b, c), GRHEAP_OK); // a self-reference
  ASSERT_EQ(grheap_store(w.heap, d, &d->a, b), GRHEAP_OK); // b is shared
  const std::set<uint64_t> before = {addr(a), addr(b), addr(c), addr(d)};

  for (int round = 0; round < 3; round++) {
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    auto * na = static_cast<Node *>(root);
    auto * nb = static_cast<Node *>(na->a);
    auto * nd = static_cast<Node *>(na->b);
    auto * nc = static_cast<Node *>(nb->a);
    EXPECT_EQ(na->id, 1u);
    EXPECT_EQ(nb->id, 2u);
    EXPECT_EQ(nc->id, 3u);
    EXPECT_EQ(nd->id, 4u);
    EXPECT_EQ(nc->a, na) << "the cycle closes on the new a";
    EXPECT_EQ(nc->b, nc) << "the self-reference names the new c";
    EXPECT_EQ(nd->a, nb) << "the shared node is one node";
    for (const void * p : {static_cast<const void *>(na), static_cast<const void *>(nb),
             static_cast<const void *>(nc), static_cast<const void *>(nd)}) {
      EXPECT_TRUE(grheap_contains(w.heap, p));
      if (round == 0) {
        EXPECT_EQ(before.count(addr(p)), 0u) << "every object has a new address";
      }
    }
  }
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST_F(RelocateTest, ALargeObjectMovesToAMappingOfItsOwnWithItsContentsIntact) {
  HeapWorld w(moving());
  void * raw = nullptr;
  const size_t size = 100000;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, size, &raw), GRHEAP_OK);
  auto * bytes = static_cast<unsigned char *>(raw);
  for (size_t i = 0; i < size; i++) {
    bytes[i] = static_cast<unsigned char>(i * 31u);
  }
  void * root = raw;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_NE(root, raw);
  bytes = static_cast<unsigned char *>(root);
  for (size_t i = 0; i < size; i++) {
    ASSERT_EQ(bytes[i], static_cast<unsigned char>(i * 31u)) << i;
  }
  size_t got = 0;
  EXPECT_EQ(grheap_size_of(w.heap, root, &got), GRHEAP_OK);
  EXPECT_EQ(got, size);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

/* ---- What stays --------------------------------------------------------- */

TEST_F(RelocateTest, APinnedObjectAndAConservativelyHeldObjectKeepTheirAddressAndOthersMove) {
  HeapWorld w(moving());
  Node * pinned = new_node(w.heap, 1, &kPlainNode);
  Node * conservative = new_node(w.heap, 2, &kPlainNode);
  Node * plain = new_node(w.heap, 3, &kPlainNode);
  ASSERT_EQ(grheap_pin(w.heap, pinned), GRHEAP_OK);
  // The conservative hit is an interior pointer in a range the source reports.
  uint64_t range_words[2] = {addr(conservative) + 8, 0};
  HeldRoots held;
  held.ranges = {range_of(range_words, 2)};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kHeldSource, &held), GRCORE_OK);
  // Both point at `plain` through traced slots, and `plain` is also a root.
  ASSERT_EQ(grheap_store(w.heap, pinned, &pinned->a, plain), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, conservative, &conservative->a, plain), GRHEAP_OK);
  void * root = plain;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  const uint64_t old_plain = addr(plain);

  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);

  EXPECT_NE(addr(root), old_plain);
  EXPECT_TRUE(grheap_contains(w.heap, pinned));
  EXPECT_TRUE(grheap_contains(w.heap, conservative));
  EXPECT_EQ(pinned->id, 1u);
  EXPECT_EQ(conservative->id, 2u);
  EXPECT_EQ(pinned->a, root) << "a pinned object's own slots are rewritten";
  EXPECT_EQ(conservative->a, root) << "so are a conservatively held one's";
  GRHEAP_RelocationStats s = reloc_stats(w);
  EXPECT_EQ(s.moved, 1u);
  EXPECT_EQ(s.stayed_pinned, 1u);
  EXPECT_EQ(s.stayed_conservative, 1u);
  ASSERT_EQ(grheap_unpin(w.heap, pinned), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

/* ---- Weak, ephemeron, IDs ------------------------------------------------ */

namespace {
struct WeakHolder {
  void * strong;
  uint64_t id;
  void * target;
};
void weak_holder_trace(GRHEAP_Tracer * t, void * payload) {
  grheap_trace_slot(t, &static_cast<WeakHolder *>(payload)->strong);
}
std::vector<std::pair<void *, uint64_t>> g_cleared;
void holder_cleared(void * payload) {
  g_cleared.emplace_back(payload, static_cast<WeakHolder *>(payload)->id);
}
const GRHEAP_WeakSpec kHolderSpec = {GRHEAP_WEAK_CELL, offsetof(WeakHolder, target), 0, 0, false, holder_cleared};
const GRHEAP_Type kHolder = GRHEAP_TYPE_INIT("holder", sizeof(WeakHolder), weak_holder_trace, nullptr, &kHolderSpec, nullptr, nullptr);

struct Pair {
  uint64_t id;
  void * key;
  void * value;
};
const GRHEAP_WeakSpec kPairSpec = {GRHEAP_EPHEMERON, 0, offsetof(Pair, key), offsetof(Pair, value), false, nullptr};
const GRHEAP_Type kPair = GRHEAP_TYPE_INIT("pair", sizeof(Pair), nullptr, nullptr, &kPairSpec, nullptr, nullptr);
} // namespace

TEST_F(RelocateTest, WeakCellsEphemeronsAndIdsAllNameTheNewAddressAndADeadReferentIsClearedAsBefore) {
  g_cleared.clear();
  HeapWorld w(moving());
  void * keeper = pinned_keeper(w);
  (void)keeper;
  Node * live = new_node(w.heap, 7, &kPlainNode);
  Node * doomed = new_node(w.heap, 8, &kPlainNode);
  Node * key = new_node(w.heap, 9, &kPlainNode);
  Node * value = new_node(w.heap, 10, &kPlainNode);
  void * hp = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kHolder, &hp), GRHEAP_OK);
  auto * holder = static_cast<WeakHolder *>(hp);
  holder->id = 100;
  holder->target = live;
  void * hq = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kHolder, &hq), GRHEAP_OK);
  auto * dying = static_cast<WeakHolder *>(hq);
  dying->id = 101;
  dying->target = doomed;
  void * pp = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kPair, &pp), GRHEAP_OK);
  auto * pair = static_cast<Pair *>(pp);
  pair->id = 102;
  pair->key = key;
  pair->value = value;
  void * roots[5] = {hp, hq, pp, live, key};
  for (void *& r : roots) {
    ASSERT_EQ(grheap_root_add(w.heap, &r), GRHEAP_OK);
  }
  uint64_t id_live = 0, id_key = 0, id_holder = 0, hash_live = 0;
  ASSERT_EQ(grheap_id_of(w.heap, live, &id_live), GRHEAP_OK);
  ASSERT_EQ(grheap_id_of(w.heap, key, &id_key), GRHEAP_OK);
  ASSERT_EQ(grheap_id_of(w.heap, hp, &id_holder), GRHEAP_OK);
  ASSERT_EQ(grheap_identity_hash(w.heap, live, &hash_live), GRHEAP_OK);
  const uint64_t old_live = addr(live), old_key = addr(key), old_holder = addr(hp);

  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);

  auto * h2 = static_cast<WeakHolder *>(roots[0]);
  auto * d2 = static_cast<WeakHolder *>(roots[1]);
  auto * p2 = static_cast<Pair *>(roots[2]);
  Node * live2 = static_cast<Node *>(roots[3]);
  Node * key2 = static_cast<Node *>(roots[4]);
  EXPECT_NE(addr(live2), old_live);
  EXPECT_NE(addr(key2), old_key);
  EXPECT_NE(addr(h2), old_holder);
  EXPECT_EQ(h2->target, live2) << "a weak slot to a live object names its new address";
  EXPECT_EQ(p2->key, key2) << "an ephemeron's key names the new address";
  EXPECT_TRUE(grheap_contains(w.heap, p2->value)) << "and its value is a live object";
  EXPECT_EQ(static_cast<Node *>(p2->value)->id, 10u);
  EXPECT_EQ(d2->target, nullptr) << "a dead referent is cleared as before";
  ASSERT_EQ(g_cleared.size(), 1u);
  EXPECT_EQ(g_cleared[0].second, 101u);
  EXPECT_EQ(g_cleared[0].first, static_cast<void *>(d2)) << "the callback sees the cell where it is now";
  // The ID table: both directions, and the identity hash.
  void * back = nullptr;
  ASSERT_EQ(grheap_object_of_id(w.heap, id_live, &back), GRHEAP_OK);
  EXPECT_EQ(back, live2);
  ASSERT_EQ(grheap_object_of_id(w.heap, id_key, &back), GRHEAP_OK);
  EXPECT_EQ(back, key2);
  ASSERT_EQ(grheap_object_of_id(w.heap, id_holder, &back), GRHEAP_OK);
  EXPECT_EQ(back, static_cast<void *>(h2));
  uint64_t again = 0, hash_again = 0;
  ASSERT_EQ(grheap_id_of(w.heap, live2, &again), GRHEAP_OK);
  EXPECT_EQ(again, id_live) << "the address index is re-keyed";
  ASSERT_EQ(grheap_identity_hash(w.heap, live2, &hash_again), GRHEAP_OK);
  EXPECT_EQ(hash_again, hash_live);
  GRHEAP_Stats st = w.stats();
  EXPECT_EQ(st.ids_live, 3u);
  // A later collection, which runs the weak registry on the new addresses.
  roots[3] = nullptr;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(static_cast<WeakHolder *>(roots[0])->target, nullptr);
  EXPECT_EQ(g_cleared.size(), 2u) << "the live cell's target died: its callback ran";
  EXPECT_EQ(g_cleared[1].second, 100u);
  for (void *& r : roots) {
    ASSERT_EQ(grheap_root_remove(w.heap, &r), GRHEAP_OK);
  }
}

/* ---- Barrier-verify ------------------------------------------------------ */

TEST_F(RelocateTest, TheBarrierVerifyShadowFollowsAMovedObjectAndItsSlots) {
  WorldConfig c = moving();
  c.verify = 1; // count
  HeapWorld w(c);
  void * keeper = pinned_keeper(w);
  (void)keeper;
  Node * a = new_node(w.heap, 1, &kPlainNode);
  Node * b = new_node(w.heap, 2, &kPlainNode);
  void * root = a;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, a, &a->a, b), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, a, &a->b, a), GRHEAP_OK);
  ASSERT_EQ(grheap_store(w.heap, b, &b->a, a), GRHEAP_OK);
  uint64_t violations = 99;
  ASSERT_EQ(grheap_verify(w.heap, &violations), GRHEAP_OK);
  ASSERT_EQ(violations, 0u);
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    ASSERT_EQ(grheap_verify(w.heap, &violations), GRHEAP_OK);
    EXPECT_EQ(violations, 0u) << "after collection " << i;
    EXPECT_EQ(w.stats().verify_violations, 0u);
  }
  // A store through the barrier at the new address is checked and recorded.
  auto * na = static_cast<Node *>(root);
  auto * nb = static_cast<Node *>(na->a);
  ASSERT_EQ(grheap_store(w.heap, nb, &nb->b, nb), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  ASSERT_EQ(grheap_verify(w.heap, &violations), GRHEAP_OK);
  EXPECT_EQ(violations, 0u);
  // And a write that bypasses the barrier is still seen, after the moves.
  na = static_cast<Node *>(root);
  na->b = nullptr;
  ASSERT_EQ(grheap_verify(w.heap, &violations), GRHEAP_OK);
  EXPECT_EQ(violations, 1u) << "verify still has teeth after a move";
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

/* ---- The detector -------------------------------------------------------- */

TEST_F(RelocateTest, ASlotReportedThroughACopyIsVisitedButNotUpdatedAndReadsPoison) {
  HeapWorld w(moving());
  void * keeper = pinned_keeper(w);
  (void)keeper;
  Node * target = new_node(w.heap, 5, &kPlainNode);
  void * troot = target;
  ASSERT_EQ(grheap_root_add(w.heap, &troot), GRHEAP_OK);
  void * hp = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kCopyReporter, &hp), GRHEAP_OK);
  auto * holder = static_cast<Node *>(hp);
  holder->a = target; // not through a store: the barrier is off here
  void * hroot = hp;
  ASSERT_EQ(grheap_root_add(w.heap, &hroot), GRHEAP_OK);
  EXPECT_EQ(read_word(&static_cast<Node *>(hp)->a), addr(target));

  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);

  auto * now = static_cast<Node *>(hroot);
  EXPECT_NE(addr(troot), addr(target)) << "the target moved";
  EXPECT_EQ(now->a, static_cast<void *>(target)) << "the reported slot kept the old address";
  EXPECT_FALSE(grheap_contains(w.heap, now->a));
  EXPECT_EQ(read_word(now->a), kPoison) << "and a read through it sees poison";
  now->a = nullptr; // so teardown does not trace the stale pointer
  ASSERT_EQ(grheap_root_remove(w.heap, &hroot), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &troot), GRHEAP_OK);
}

TEST_F(RelocateTest, ARootSourceThatReadsAndDoesNotWriteLeavesAStaleSlotThatReadsPoison) {
  HeapWorld w(moving());
  void * keeper = pinned_keeper(w);
  (void)keeper;
  Node * n = new_node(w.heap, 6, &kPlainNode);
  CopyRoots roots;
  roots.words = {addr(n)};
  ASSERT_EQ(grcore_context_add_root_source(w.ctx, &kCopySource, &roots), GRCORE_OK);
  const uint64_t old = addr(n);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(w.stats().live_objects, 2u) << "the copy kept it alive";
  EXPECT_EQ(reloc_stats(w).moved, 1u);
  EXPECT_FALSE(grheap_contains(w.heap, reinterpret_cast<void *>(old)));
  EXPECT_EQ(read_word(reinterpret_cast<void *>(old)), kPoison)
      << "the address the source still holds is poisoned";
}

TEST_F(RelocateTest, AFieldTheTraceOmitsIsAStalePointerAndReadsPoisonAfterTheMove) {
  HeapWorld w(moving());
  void * keeper = pinned_keeper(w);
  (void)keeper;
  Node * target = new_node(w.heap, 4, &kPlainNode);
  void * troot = target;
  ASSERT_EQ(grheap_root_add(w.heap, &troot), GRHEAP_OK);
  void * hp = nullptr;
  ASSERT_EQ(grheap_alloc(w.heap, &kOmitter, &hp), GRHEAP_OK);
  auto * holder = static_cast<Node *>(hp);
  holder->b = target; // the engine field `trace` leaves out
  void * hroot = hp;
  ASSERT_EQ(grheap_root_add(w.heap, &hroot), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  auto * now = static_cast<Node *>(hroot);
  EXPECT_EQ(now->b, static_cast<void *>(target)) << "untraced, so never rewritten";
  EXPECT_EQ(read_word(now->b), kPoison);
  now->b = nullptr;
  ASSERT_EQ(grheap_root_remove(w.heap, &hroot), GRHEAP_OK);
  ASSERT_EQ(grheap_root_remove(w.heap, &troot), GRHEAP_OK);
}

TEST_F(RelocateTest, TheOldCellIsFreedAndPoisonedAndItsDestroyHookIsNotRun) {
  HeapWorld w(moving());
  void * keeper = pinned_keeper(w);
  (void)keeper;
  Node * live = new_node(w.heap, 21, &kNode);   // kNode logs its destroy
  Node * dead = new_node(w.heap, 22, &kNode);
  (void)dead;
  void * root = live;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  const uint64_t old = addr(live);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_FALSE(destroyed(21)) << "the object lives on: its destroy does not run";
  EXPECT_TRUE(destroyed(22)) << "a dead one is destroyed once, as before";
  EXPECT_EQ(destroyed_count(22), 1u);
  EXPECT_EQ(read_word(reinterpret_cast<void *>(old)), kPoison);
  EXPECT_EQ(read_word(reinterpret_cast<unsigned char *>(old) + 8), kPoison);
  EXPECT_EQ(static_cast<Node *>(root)->id, 21u);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

/* ---- Failures ------------------------------------------------------------- */

TEST_F(RelocateTest, WhenNoCellCanBeHadTheObjectStaysWhereItIsAndIsCounted) {
  HeapWorld w(moving());
  const size_t size = 70000; // one mapping each, so a move needs a new mapping
  void * raw = nullptr;
  ASSERT_EQ(grheap_alloc_sized(w.heap, &kLeaf, size, &raw), GRHEAP_OK);
  std::memset(raw, 0x5A, size);
  void * root = raw;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  w.pages.fail = true;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  w.pages.fail = false;
  EXPECT_EQ(root, raw) << "nothing is half moved";
  EXPECT_EQ(static_cast<unsigned char *>(root)[size - 1], 0x5A);
  GRHEAP_RelocationStats s = reloc_stats(w);
  EXPECT_EQ(s.moved, 0u);
  EXPECT_EQ(s.stayed_no_cell, 1u);
  // The next collection, with pages again, moves it.
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_NE(root, raw);
  EXPECT_EQ(static_cast<unsigned char *>(root)[size - 1], 0x5A);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST_F(RelocateTest, WhenTheTableCannotBeHadNothingMovesInThatCollection) {
  HeapWorld w(moving());
  Node * n = new_node(w.heap, 1, &kPlainNode);
  void * root = n;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  w.allocator.fail_all = true;
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  w.allocator.fail_all = false;
  EXPECT_EQ(root, static_cast<void *>(n));
  EXPECT_EQ(reloc_stats(w).skipped, 1u);
  EXPECT_EQ(reloc_stats(w).moved, 0u);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_NE(root, static_cast<void *>(n));
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

/* ---- Images ---------------------------------------------------------------- */

namespace {
struct Box {
  std::vector<uint64_t> words;
};
void box_enumerate(GRCORE_Context *, void * value, const GRCORE_RootVisitor * visitor) {
  for (uint64_t & w : static_cast<Box *>(value)->words) {
    visitor->slot(visitor->user, &w);
  }
}
const GRCORE_RootSource kBox = GRCORE_ROOT_SOURCE_INIT("box", box_enumerate);
const GRHEAP_Type * resolve_plain(void *, const char * name) {
  return std::strcmp(name, kPlainNode.name) == 0 ? &kPlainNode : nullptr;
}
} // namespace

TEST_F(RelocateTest, AnImageIsTakenAndRestoredWithoutAMoveAndTheGraphSurvivesBothHeapsMoving) {
  HeapWorld src(moving());
  Box sbox;
  ASSERT_EQ(grcore_context_add_root_source(src.ctx, &kBox, &sbox), GRCORE_OK);
  Node * a = new_node(src.heap, 1, &kPlainNode);
  Node * b = new_node(src.heap, 2, &kPlainNode);
  sbox.words = {addr(a)};
  ASSERT_EQ(grheap_store(src.heap, a, &a->a, b), GRHEAP_OK);
  ASSERT_EQ(grheap_store(src.heap, b, &b->a, a), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(src.heap), GRHEAP_OK); // it has moved before
  const uint64_t moved_before = reloc_stats(src).moved;
  const uint64_t root_before = sbox.words[0];

  GRHEAP_Image * image = nullptr;
  ASSERT_EQ(grheap_image_take(src.heap, nullptr, &image), GRHEAP_OK);
  EXPECT_EQ(reloc_stats(src).moved, moved_before) << "no move during a take";
  EXPECT_EQ(sbox.words[0], root_before);

  HeapWorld dst(moving());
  Box dbox;
  dbox.words = {0};
  ASSERT_EQ(grcore_context_add_root_source(dst.ctx, &kBox, &dbox), GRCORE_OK);
  GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT(nullptr, nullptr);
  env.resolve = resolve_plain;
  ASSERT_EQ(grheap_image_instantiate(dst.heap, image, &env), GRHEAP_OK);
  EXPECT_EQ(reloc_stats(dst).moved, 0u) << "no move during a restore";
  grheap_image_release(image);

  for (HeapWorld * w : {&src, &dst}) {
    Box & box = w == &src ? sbox : dbox;
    for (int i = 0; i < 3; i++) {
      ASSERT_EQ(grheap_collect(w->heap), GRHEAP_OK);
      auto * na = reinterpret_cast<Node *>(box.words[0]);
      ASSERT_TRUE(grheap_contains(w->heap, na));
      EXPECT_EQ(na->id, 1u);
      auto * nb = static_cast<Node *>(na->a);
      EXPECT_EQ(nb->id, 2u);
      EXPECT_EQ(nb->a, static_cast<void *>(na));
    }
    EXPECT_GT(reloc_stats(*w).moved, 0u);
  }
}

/* ---- Switching it on --------------------------------------------------------- */

TEST_F(RelocateTest, TheModeIsOffUntilSwitchedOnAndTheEnvironmentIsTheDefaultAnExplicitSetterWins) {
  EnvGuard off("GRHEAP_RELOCATE", nullptr);
  GRHEAP_Options * o = nullptr;
  ASSERT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK);
  EXPECT_FALSE(grheap_options_get_relocate(o));
  EXPECT_FALSE(grheap_options_get_relocate(nullptr));
  EXPECT_EQ(grheap_options_set_relocate(nullptr, true), GRHEAP_ERR_INVALID);
  ASSERT_EQ(grheap_options_set_relocate(o, true), GRHEAP_OK);
  EXPECT_TRUE(grheap_options_get_relocate(o));
  grheap_options_destroy(o);
  {
    EnvGuard on("GRHEAP_RELOCATE", "1");
    ASSERT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK);
    EXPECT_TRUE(grheap_options_get_relocate(o));
    ASSERT_EQ(grheap_options_set_relocate(o, false), GRHEAP_OK);
    EXPECT_FALSE(grheap_options_get_relocate(o)) << "an explicit setter wins";
    grheap_options_destroy(o);
  }
  // A heap with relocation off never moves.
  WorldConfig c = moving();
  c.relocate = 0;
  HeapWorld w(c);
  Node * n = new_node(w.heap, 1, &kPlainNode);
  void * root = n;
  ASSERT_EQ(grheap_root_add(w.heap, &root), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  EXPECT_EQ(root, static_cast<void *>(n));
  EXPECT_EQ(reloc_stats(w).collections, 0u);
  ASSERT_EQ(grheap_root_remove(w.heap, &root), GRHEAP_OK);
}

TEST_F(RelocateTest, UnderTortureEveryAllocationIsAMove) {
  WorldConfig c = moving();
  c.torture = 1;
  HeapWorld w(c);
  Node * first = new_node(w.heap, 0, &kPlainNode);
  void * head = first;
  ASSERT_EQ(grheap_root_add(w.heap, &head), GRHEAP_OK);
  std::set<uint64_t> seen = {addr(first)};
  for (uint64_t i = 1; i <= 40; i++) {
    uint64_t before = addr(head);
    void * p = nullptr;
    ASSERT_EQ(grheap_alloc(w.heap, &kPlainNode, &p), GRHEAP_OK);
    EXPECT_NE(addr(head), before) << "the allocation collected first, and the root moved";
    auto * n = static_cast<Node *>(p);
    n->id = i;
    ASSERT_EQ(grheap_store(w.heap, n, &n->a, head), GRHEAP_OK);
    head = n;
  }
  uint64_t count = 0;
  for (auto * n = static_cast<Node *>(head); n != nullptr; n = static_cast<Node *>(n->a)) {
    EXPECT_EQ(n->id, 40 - count);
    count++;
  }
  EXPECT_EQ(count, 41u);
  EXPECT_GE(reloc_stats(w).moved, 40u);
  ASSERT_EQ(grheap_root_remove(w.heap, &head), GRHEAP_OK);
}

TEST_F(RelocateTest, ARandomGraphReadsTheSameThroughManyCollections) {
  HeapWorld w(moving());
  void * keeper = pinned_keeper(w);
  (void)keeper;
  constexpr int kN = 300;
  std::vector<Node *> nodes;
  std::vector<int> ea(kN), eb(kN);
  uint64_t s = 12345;
  auto next = [&]() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  };
  // Every node held by a handle while the graph is built, then only node 0.
  std::vector<GRHEAP_Handle *> handles;
  for (int i = 0; i < kN; i++) {
    Node * n = new_node(w.heap, static_cast<uint64_t>(i), &kPlainNode);
    nodes.push_back(n);
    GRHEAP_Handle * h = nullptr;
    ASSERT_EQ(grheap_handle_create(w.heap, n, &h), GRHEAP_OK);
    handles.push_back(h);
  }
  for (int i = 0; i < kN; i++) {
    ea[i] = static_cast<int>(next() % kN);
    eb[i] = static_cast<int>(next() % 3 == 0 ? -1 : next() % kN);
    ASSERT_EQ(grheap_store(w.heap, nodes[i], &nodes[i]->a, nodes[ea[i]]), GRHEAP_OK);
    if (eb[i] >= 0) {
      ASSERT_EQ(grheap_store(w.heap, nodes[i], &nodes[i]->b, nodes[eb[i]]), GRHEAP_OK);
    }
  }
  for (int round = 0; round < 8; round++) {
    ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
    for (int i = 0; i < kN; i++) {
      auto * n = static_cast<Node *>(grheap_handle_get(handles[i]));
      ASSERT_NE(n, nullptr);
      ASSERT_EQ(n->id, static_cast<uint64_t>(i));
      ASSERT_EQ(n->a, grheap_handle_get(handles[ea[i]])) << round << " " << i;
      void * want_b = eb[i] >= 0 ? grheap_handle_get(handles[eb[i]]) : nullptr;
      ASSERT_EQ(n->b, want_b) << round << " " << i;
    }
  }
  EXPECT_GE(reloc_stats(w).moved, static_cast<uint64_t>(kN) * 8u);
  for (GRHEAP_Handle * h : handles) {
    ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
