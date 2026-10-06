/**
 * @file
 *
 * Heap images (image.h): taking one from the objects a context's roots reach,
 * instantiating it into a fresh heap, and the heap key's snapshot hooks.
 *
 * The roots are a root source the test owns (a box of words), because a heap
 * with a registered C root refuses to be imaged and an engine's frames are not
 * here. Objects are compared by identity numbers the test stored in them, and
 * the comparison itself is shown to fail on a planted difference.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <ghoti.io/runtime-core/b/snapshot.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>

namespace {

/* ---- the roots a test owns ---------------------------------------------- */

struct RootBox {
  std::vector<uint64_t> words;
};

void box_enumerate(
    GRCORE_Context *, void * value, const GRCORE_RootVisitor * visitor) {
  auto * box = static_cast<RootBox *>(value);
  if (visitor->slot != nullptr) {
    for (uint64_t & w : box->words) {
      visitor->slot(visitor->user, &w);
    }
  }
}
const GRCORE_RootSource kBoxSource = GRCORE_ROOT_SOURCE_INIT("box", box_enumerate);

/* A source with a conservative range, for the refusal. */
struct RangeBox {
  uint64_t cell = 0;
  bool empty = false;
};
void range_enumerate(
    GRCORE_Context *, void * value, const GRCORE_RootVisitor * visitor) {
  auto * r = static_cast<RangeBox *>(value);
  if (visitor->range != nullptr) {
    uint64_t lo = reinterpret_cast<uint64_t>(&r->cell);
    GRCORE_ConservativeRange range = {lo, r->empty ? lo : lo + 8, UINT64_MAX, 0, 0};
    visitor->range(visitor->user, &range);
  }
}
const GRCORE_RootSource kRangeSource = GRCORE_ROOT_SOURCE_INIT("range", range_enumerate);

/* ---- types with host pointers --------------------------------------------- */

/* A label table: the host's names for things, which an object may point at. */
struct Labels {
  std::vector<const char *> names;
};
const char * const kA[] = {"red", "green", "blue"};
const char * const kB[] = {"blue", "red", "green"};

struct Named {
  void * next;
  const char * label; // a host pointer into a Labels table
  uint64_t n;
};
void named_trace(GRHEAP_Tracer * t, void * payload) {
  grheap_trace_slot(t, &static_cast<Named *>(payload)->next);
}
GRHEAP_Result named_snapshot(
    GRCORE_Context *, void * payload, GRHEAP_ImageWriter * w) {
  auto * n = static_cast<Named *>(payload);
  GRHEAP_Result r = grheap_image_writer_string(w, n->label);
  n->label = nullptr;
  return r;
}
GRHEAP_Result named_restore(
    GRCORE_Context *, void * payload, GRHEAP_ImageReader * r, void * user) {
  const char * text;
  GRHEAP_Result res = grheap_image_reader_string(r, &text, nullptr);
  if (res != GRHEAP_OK) {
    return res;
  }
  auto * n = static_cast<Named *>(payload);
  if (*text == '\0') {
    return GRHEAP_OK;
  }
  for (const char * name : static_cast<Labels *>(user)->names) {
    if (std::strcmp(name, text) == 0) {
      n->label = name;
      return GRHEAP_OK;
    }
  }
  return GRHEAP_ERR_INVALID; // a label the destination does not have
}
uint64_t g_named_destroyed = 0;
void named_destroy(void *) { g_named_destroyed++; }
const GRHEAP_Type kNamed = GRHEAP_TYPE_INIT("named", sizeof(Named), named_trace, named_destroy,
    nullptr, named_snapshot, named_restore);
/* The same layout with no hooks: the type that forgot, a control. */
const GRHEAP_Type kNamedForgot = GRHEAP_TYPE_INIT("named-forgot", sizeof(Named), named_trace,
    nullptr, nullptr, nullptr, nullptr);

/* A type whose restore refuses payloads with n == 3. */
GRHEAP_Result flaky_restore(
    GRCORE_Context *, void * payload, GRHEAP_ImageReader *, void *) {
  return static_cast<Named *>(payload)->n == 3 ? GRHEAP_ERR_INVALID : GRHEAP_OK;
}
GRHEAP_Result flaky_snapshot(GRCORE_Context *, void *, GRHEAP_ImageWriter * w) {
  return grheap_image_writer_string(w, "x");
}
/* What a destroy hook finds in a payload during a failed restore: the pointers
 * `snapshot` cleared, which are cleared until a `restore` stores a resolved
 * one (type.h). It must be able to take that, not only a restored payload. */
std::vector<const char *> g_flaky_seen;
void flaky_destroy(void * payload) {
  g_flaky_seen.push_back(static_cast<Named *>(payload)->label);
  g_named_destroyed++;
}
const GRHEAP_Type kFlaky = GRHEAP_TYPE_INIT("flaky", sizeof(Named), named_trace, flaky_destroy,
    nullptr, flaky_snapshot, flaky_restore);

/* A type whose restore reads past what its snapshot wrote. */
GRHEAP_Result g_reader_seen[3];
GRHEAP_Result reader_snapshot(GRCORE_Context *, void *, GRHEAP_ImageWriter * w) {
  return grheap_image_writer_write(w, "ab", 2); // no terminator
}
GRHEAP_Result reader_restore(
    GRCORE_Context *, void *, GRHEAP_ImageReader * r, void *) {
  const char * text = nullptr;
  g_reader_seen[0] = grheap_image_reader_string(r, &text, nullptr);
  char buf[8];
  g_reader_seen[1] = grheap_image_reader_read(r, buf, sizeof buf);
  g_reader_seen[2] = grheap_image_reader_read(r, buf, 1);
  return g_reader_seen[0];
}
const GRHEAP_Type kReader = GRHEAP_TYPE_INIT("reader", sizeof(Named), named_trace, nullptr,
    nullptr, reader_snapshot, reader_restore);

/* A type that reports a slot outside its payload. */
uint64_t g_outside = 0;
void outside_trace(GRHEAP_Tracer * t, void *) {
  grheap_trace_word(t, &g_outside);
}
const GRHEAP_Type kOutside = GRHEAP_TYPE_INIT("outside", 16, outside_trace, nullptr, nullptr,
    nullptr, nullptr);

/* A weak cell. */
struct WCell {
  void * target;
};
const GRHEAP_WeakSpec kWCellSpec = {
    GRHEAP_WEAK_CELL, offsetof(WCell, target), 0, 0, false, nullptr};
const GRHEAP_Type kWCell = GRHEAP_TYPE_INIT("wcell", sizeof(WCell), nullptr, nullptr, &kWCellSpec,
    nullptr, nullptr);

/* Two types with one name. */
const GRHEAP_Type kTwinOne = GRHEAP_TYPE_INIT("twin", 16, nullptr, nullptr, nullptr, nullptr, nullptr);
const GRHEAP_Type kTwinTwo = GRHEAP_TYPE_INIT("twin", 24, nullptr, nullptr, nullptr, nullptr, nullptr);
const GRHEAP_Type kNameless = GRHEAP_TYPE_INIT(nullptr, 16, nullptr, nullptr, nullptr, nullptr, nullptr);

const GRHEAP_Type * const kKnown[] = {&kNode, &kPlainNode, &kLeaf, &kNamed,
    &kNamedForgot, &kFlaky, &kWCell, &kOutside};

const GRHEAP_Type * resolve_known(void *, const char * name) {
  for (const GRHEAP_Type * t : kKnown) {
    if (std::strcmp(t->name, name) == 0) {
      return t;
    }
  }
  return nullptr;
}

const GRHEAP_Type * resolve_nothing(void *, const char *) { return nullptr; }

/* ---- a world with a box of roots -------------------------------------------- */

struct ImgWorld {
  RootBox box;
  HeapWorld w;
  // Relocation is off in these worlds: the tests keep the nodes they build in C
  // variables. (test_relocate.cpp runs an image through relocating heaps.)
  explicit ImgWorld(size_t slots, const WorldConfig & config = WorldConfig())
      : w(unmoved(config)) {
    box.words.assign(slots, 0);
    EXPECT_EQ(grcore_context_add_root_source(w.ctx, &kBoxSource, &box), GRCORE_OK);
  }
  void set(size_t i, const void * p) { box.words[i] = addr(p); }
};

struct Taken {
  GRHEAP_Image * image = nullptr;
  ~Taken() { grheap_image_release(image); }
};

GRHEAP_RestoreEnv env_with(void * user = nullptr) {
  GRHEAP_RestoreEnv e = GRHEAP_RESTORE_ENV_INIT(nullptr, nullptr);
  e.resolve = resolve_known;
  e.user = user;
  return e;
}

/* Every 8-byte window of `bytes` (aligned or not) equal to `word`. */
bool holds_word(const void * bytes, size_t size, uint64_t word) {
  const unsigned char * p = static_cast<const unsigned char *>(bytes);
  for (size_t i = 0; i + 8 <= size; i++) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    if (w == word) {
      return true;
    }
  }
  return false;
}

/* The graph a test builds, as a model: nodes numbered by their `id`. */
struct Model {
  std::vector<int> a, b;     // edge targets, -1 for none
  std::vector<int> root_of;  // for each root word, a node or -1
  std::vector<bool> reach() const {
    std::vector<bool> seen(a.size(), false);
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
    while (!stack.empty()) {
      int i = stack.back();
      stack.pop_back();
      push(a[static_cast<size_t>(i)]);
      push(b[static_cast<size_t>(i)]);
    }
    return seen;
  }
};

/* Builds `n` nodes on `w` (each rooted in its own word as it is made, so a
 * torture heap keeps it), writes edges through the barrier, then roots
 * exactly the model's roots and drops the rest. Slots 0..n-1 of the box hold
 * the nodes while building; afterwards the box holds `model.root_of`. */
void build(ImgWorld & w, Model & m, uint64_t seed, int n, int roots,
    const GRHEAP_Type * type = &kPlainNode) {
  uint64_t s = seed * 0x9E3779B97F4A7C15ull + 1;
  auto next = [&]() {
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return (s * 0x2545F4914F6CDD1Dull) >> 11;
  };
  w.box.words.assign(static_cast<size_t>(n > roots ? n : roots), 0);
  m.a.assign(static_cast<size_t>(n), -1);
  m.b.assign(static_cast<size_t>(n), -1);
  std::vector<Node *> nodes;
  for (int i = 0; i < n; i++) {
    Node * node = new_node(w.w.heap, static_cast<uint64_t>(i), type);
    w.set(static_cast<size_t>(i), node);
    nodes.push_back(node);
  }
  for (int i = 0; i < n; i++) {
    if (next() % 4 != 0) {
      int t = static_cast<int>(next() % static_cast<uint64_t>(n));
      m.a[static_cast<size_t>(i)] = t;
      ASSERT_EQ(grheap_store(w.w.heap, nodes[static_cast<size_t>(i)],
                    &nodes[static_cast<size_t>(i)]->a, nodes[static_cast<size_t>(t)]),
          GRHEAP_OK);
    }
    if (next() % 3 != 0) {
      int t = static_cast<int>(next() % static_cast<uint64_t>(n));
      m.b[static_cast<size_t>(i)] = t;
      ASSERT_EQ(grheap_store(w.w.heap, nodes[static_cast<size_t>(i)],
                    &nodes[static_cast<size_t>(i)]->b, nodes[static_cast<size_t>(t)]),
          GRHEAP_OK);
    }
  }
  m.root_of.assign(w.box.words.size(), -1);
  for (int r = 0; r < roots; r++) {
    m.root_of[static_cast<size_t>(r)] = static_cast<int>(next() % static_cast<uint64_t>(n));
  }
  for (size_t i = 0; i < w.box.words.size(); i++) {
    w.box.words[i] = m.root_of[i] < 0 ? 0 : addr(nodes[static_cast<size_t>(m.root_of[i])]);
  }
}

/* Compares what `w` holds with the model: every root word names the model's
 * node, every reachable node has the model's edges and `id`, and exactly the
 * reachable nodes are there. Returns an empty string, or what differs. */
std::string compare(ImgWorld & w, const Model & m, bool collect_first) {
  if (collect_first) {
    EXPECT_EQ(grheap_collect(w.w.heap), GRHEAP_OK);
  }
  std::map<const Node *, int> ids;
  std::vector<const Node *> stack;
  if (w.box.words.size() != m.root_of.size()) {
    return "root count";
  }
  for (size_t i = 0; i < w.box.words.size(); i++) {
    const Node * n = reinterpret_cast<const Node *>(w.box.words[i]);
    if ((n == nullptr) != (m.root_of[i] < 0)) {
      return "root " + std::to_string(i) + " null-ness";
    }
    if (n != nullptr) {
      if (!grheap_contains(w.w.heap, n)) {
        return "root " + std::to_string(i) + " is not an object";
      }
      if (static_cast<int>(n->id) != m.root_of[i]) {
        return "root " + std::to_string(i) + " names the wrong node";
      }
      if (ids.emplace(n, static_cast<int>(n->id)).second) {
        stack.push_back(n);
      }
    }
  }
  while (!stack.empty()) {
    const Node * n = stack.back();
    stack.pop_back();
    size_t id = static_cast<size_t>(n->id);
    const void * edges[2] = {n->a, n->b};
    const int want[2] = {m.a[id], m.b[id]};
    for (int k = 0; k < 2; k++) {
      const Node * t = static_cast<const Node *>(edges[k]);
      if ((t == nullptr) != (want[k] < 0)) {
        return "node " + std::to_string(id) + " edge " + std::to_string(k) + " null-ness";
      }
      if (t == nullptr) {
        continue;
      }
      if (!grheap_contains(w.w.heap, t)) {
        return "node " + std::to_string(id) + " edge is not an object";
      }
      if (static_cast<int>(t->id) != want[k]) {
        return "node " + std::to_string(id) + " edge " + std::to_string(k) +
            " goes to the wrong node";
      }
      if (ids.emplace(t, static_cast<int>(t->id)).second) {
        stack.push_back(t);
      }
    }
  }
  std::vector<bool> r = m.reach();
  size_t reachable = 0;
  for (bool b : r) {
    reachable += b;
  }
  if (ids.size() != reachable) {
    return "reachable count " + std::to_string(ids.size()) + " != " +
        std::to_string(reachable);
  }
  for (size_t i = 0; i < r.size(); i++) {
    bool found = false;
    for (auto & p : ids) {
      found = found || static_cast<size_t>(p.second) == i;
    }
    if (found != static_cast<bool>(r[i])) {
      return "node " + std::to_string(i) + " presence";
    }
  }
  return "";
}

class ImageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    destroyed_log().clear();
    g_named_destroyed = 0;
  }
};

size_t reach_count(const Model & m) {
  size_t n = 0;
  for (bool b : m.reach()) {
    n += b;
  }
  return n;
}

} // namespace

/* ---- round trips ------------------------------------------------------------- */

TEST_F(ImageTest, AGraphWithCyclesSharingAndNullsRoundTripsToTheSameShape) {
  ImgWorld src(4);
  Node * a = new_node(src.w.heap, 0, &kPlainNode);
  src.set(0, a);
  Node * b = new_node(src.w.heap, 1, &kPlainNode);
  src.set(1, b);
  Node * c = new_node(src.w.heap, 2, &kPlainNode);
  src.set(2, c);
  grheap_store(src.w.heap, a, &a->a, b);
  grheap_store(src.w.heap, a, &a->b, c);
  grheap_store(src.w.heap, b, &b->a, a); // a cycle
  grheap_store(src.w.heap, b, &b->b, c); // c is shared
  src.box.words[1] = 0;                  // b only through a
  src.box.words[2] = 0;
  src.box.words[3] = addr(c);            // c also a root
  Model m;
  m.a = {1, 0, -1};
  m.b = {2, 2, -1};
  m.root_of = {0, -1, -1, 2};

  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  EXPECT_EQ(grheap_image_object_count(t.image), 3u);
  EXPECT_EQ(grheap_image_root_count(t.image), 4u);
  EXPECT_EQ(grheap_image_refcount(t.image), 1u);
  EXPECT_GT(grheap_image_size(t.image), 3u * 32u);

  ImgWorld dst(4);
  GRHEAP_RestoreEnv env = env_with();
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  EXPECT_EQ(compare(dst, m, false), "");
  EXPECT_EQ(dst.w.stats().live_objects, 3u);
  for (uint64_t word : dst.box.words) {
    EXPECT_NE(word, src.box.words[0]) << "a fresh object, not the old address";
  }
  // The source is untouched, and the shape holds after a collection too.
  EXPECT_EQ(compare(src, m, true), "");
  EXPECT_EQ(compare(dst, m, true), "");
}

TEST_F(ImageTest, AnEmptyHeapAndAHeapOfOnlyPlainRootWordsRoundTrip) {
  ImgWorld src(3);
  src.box.words = {0, 0x11, 0x22}; // not pointers into the heap: plain words
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  EXPECT_EQ(grheap_image_object_count(t.image), 0u);
  EXPECT_EQ(grheap_image_root_count(t.image), 3u);
  ImgWorld dst(3);
  GRHEAP_RestoreEnv env = env_with();
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  EXPECT_EQ(dst.box.words, (std::vector<uint64_t>{0, 0, 0})) << "a stale word is not carried";
  EXPECT_EQ(dst.w.stats().live_objects, 0u);
}

TEST_F(ImageTest, AnImageHoldsNoAddressAndTheControlThatForgotItsHookDoes) {
  // The scan that proves "no address in an image", and the type it must catch.
  static const char * const label = "red";
  ImgWorld src(2);
  void * p = nullptr;
  ASSERT_EQ(grheap_alloc(src.w.heap, &kNamed, &p), GRHEAP_OK);
  src.set(0, p);
  auto * hooked = static_cast<Named *>(p);
  hooked->label = label;
  hooked->n = 7;
  ASSERT_EQ(grheap_alloc(src.w.heap, &kNamedForgot, &p), GRHEAP_OK);
  src.set(1, p);
  auto * forgot = static_cast<Named *>(p);
  forgot->label = label;
  forgot->n = 8;
  hooked->next = forgot;

  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  size_t size;
  const void * bytes = grheap_image_bytes(t.image, &size);
  // The hooked object's pointer is gone; the forgetful one's is in the image.
  EXPECT_TRUE(holds_word(bytes, size, addr(label)))
      << "the control: a type with no hook leaves its host pointer in the image";
  // Taking the hooked object alone leaves none.
  ImgWorld only(1);
  ASSERT_EQ(grheap_alloc(only.w.heap, &kNamed, &p), GRHEAP_OK);
  only.set(0, p);
  static_cast<Named *>(p)->label = label;
  Taken t2;
  ASSERT_EQ(grheap_image_take(only.w.heap, nullptr, &t2.image), GRHEAP_OK);
  bytes = grheap_image_bytes(t2.image, &size);
  EXPECT_FALSE(holds_word(bytes, size, addr(label)));
  EXPECT_FALSE(holds_word(bytes, size, addr(p)));
  EXPECT_FALSE(holds_word(bytes, size, addr(&kNamed))) << "a type is its name";
  EXPECT_NE(std::string(static_cast<const char *>(bytes), size).find("named"),
      std::string::npos);
}

TEST_F(ImageTest, NoSourceAddressSurvivesInAnImageOfARandomGraph) {
  for (uint64_t seed = 1; seed <= 25; seed++) {
    ImgWorld src(1);
    Model m;
    build(src, m, seed, 40, 5);
    Taken t;
    ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
    size_t size;
    const void * bytes = grheap_image_bytes(t.image, &size);
    for (uint64_t word : src.box.words) {
      if (word != 0) {
        EXPECT_FALSE(holds_word(bytes, size, word)) << "seed " << seed;
      }
    }
    // Every object's address: walk the source graph.
    std::set<const Node *> seen;
    std::vector<const Node *> stack;
    for (uint64_t word : src.box.words) {
      if (word != 0) {
        stack.push_back(reinterpret_cast<const Node *>(word));
      }
    }
    while (!stack.empty()) {
      const Node * n = stack.back();
      stack.pop_back();
      if (!seen.insert(n).second) {
        continue;
      }
      EXPECT_FALSE(holds_word(bytes, size, addr(n))) << "seed " << seed;
      for (const void * e : {n->a, n->b}) {
        if (e != nullptr) {
          stack.push_back(static_cast<const Node *>(e));
        }
      }
    }
  }
}

TEST_F(ImageTest, ARandomisedGraphRoundTripsAgainstTheModelInEveryModePair) {
  struct Pair {
    GRHEAP_Mode from, to;
  };
  const Pair pairs[] = {{GRHEAP_MODE_COLLECTED, GRHEAP_MODE_COLLECTED},
      {GRHEAP_MODE_COLLECTED, GRHEAP_MODE_ARENA},
      {GRHEAP_MODE_ARENA, GRHEAP_MODE_COLLECTED},
      {GRHEAP_MODE_ARENA, GRHEAP_MODE_ARENA}};
  for (const Pair & pair : pairs) {
    for (uint64_t seed = 1; seed <= 60; seed++) {
      WorldConfig from, to;
      from.mode = pair.from;
      to.mode = pair.to;
      ImgWorld src(1, from);
      Model m;
      int n = 5 + static_cast<int>(seed % 60);
      build(src, m, seed, n, 1 + static_cast<int>(seed % 5));
      Taken t;
      ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
      // Unreachable nodes are not in the image: it holds exactly what a
      // collection would keep.
      ASSERT_EQ(grheap_image_object_count(t.image), reach_count(m)) << "seed " << seed;
      ImgWorld dst(src.box.words.size(), to);
      GRHEAP_RestoreEnv env = env_with();
      ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK)
          << "seed " << seed;
      ASSERT_EQ(compare(dst, m, pair.to == GRHEAP_MODE_COLLECTED), "") << "seed " << seed;
      ASSERT_EQ(compare(src, m, pair.from == GRHEAP_MODE_COLLECTED), "") << "seed " << seed;
      if (pair.to == GRHEAP_MODE_COLLECTED) {
        EXPECT_EQ(dst.w.stats().live_objects, reach_count(m)) << "seed " << seed;
      }
    }
  }
}

TEST_F(ImageTest, TheComparisonSeesAPlantedDifference) {
  // A control for `compare`: change one edge of the restored graph and the
  // comparison must say so, or every test above proves nothing.
  ImgWorld src(1);
  Model m;
  build(src, m, 7, 30, 3);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  ImgWorld dst(src.box.words.size());
  GRHEAP_RestoreEnv env = env_with();
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  ASSERT_EQ(compare(dst, m, false), "");
  Node * root = nullptr;
  for (uint64_t word : dst.box.words) {
    if (word != 0) {
      root = reinterpret_cast<Node *>(word);
      break;
    }
  }
  ASSERT_NE(root, nullptr);
  void * other = root->a == root ? nullptr : static_cast<void *>(root);
  ASSERT_EQ(grheap_store(dst.w.heap, root, &root->a, other), GRHEAP_OK);
  EXPECT_NE(compare(dst, m, false), "") << "the planted edge went unnoticed";
}

/* ---- stable IDs ----------------------------------------------------------------- */

TEST_F(ImageTest, StableIdsSurviveAndLaterIdsDoNotCollide) {
  ImgWorld src(3);
  Node * a = new_node(src.w.heap, 0, &kPlainNode);
  src.set(0, a);
  Node * b = new_node(src.w.heap, 1, &kPlainNode);
  src.set(1, b);
  Node * dead = new_node(src.w.heap, 2, &kPlainNode);
  src.set(2, dead);
  uint64_t ida, idb, iddead;
  ASSERT_EQ(grheap_id_of(src.w.heap, a, &ida), GRHEAP_OK);
  ASSERT_EQ(grheap_id_of(src.w.heap, dead, &iddead), GRHEAP_OK);
  ASSERT_EQ(grheap_id_of(src.w.heap, b, &idb), GRHEAP_OK);
  src.box.words[2] = 0; // `dead` is unreachable, its id is in the heap still
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);

  ImgWorld dst(3);
  GRHEAP_RestoreEnv env = env_with();
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  void * got;
  ASSERT_EQ(grheap_object_of_id(dst.w.heap, ida, &got), GRHEAP_OK);
  EXPECT_EQ(static_cast<Node *>(got)->id, 0u);
  ASSERT_EQ(grheap_object_of_id(dst.w.heap, idb, &got), GRHEAP_OK);
  EXPECT_EQ(static_cast<Node *>(got)->id, 1u);
  EXPECT_EQ(grheap_object_of_id(dst.w.heap, iddead, &got), GRHEAP_ERR_INVALID)
      << "an object that was not reachable has no id after the restore";
  // The identity hash is a function of the id, so it is the same.
  uint64_t h_src, h_dst;
  ASSERT_EQ(grheap_identity_hash(src.w.heap, a, &h_src), GRHEAP_OK);
  ASSERT_EQ(grheap_identity_hash(dst.w.heap, reinterpret_cast<void *>(dst.box.words[0]), &h_dst),
      GRHEAP_OK);
  EXPECT_EQ(h_src, h_dst);
  // The next id issued is above every id the source ever issued.
  Node * fresh = new_node(dst.w.heap, 9, &kPlainNode);
  dst.box.words[2] = addr(fresh);
  uint64_t idfresh;
  ASSERT_EQ(grheap_id_of(dst.w.heap, fresh, &idfresh), GRHEAP_OK);
  EXPECT_GT(idfresh, iddead);
  EXPECT_GT(idfresh, ida);
  EXPECT_GT(idfresh, idb);
  EXPECT_EQ(dst.w.stats().ids_live, 3u);
}

/* ---- the instruments over a restored heap ------------------------------------------ */

TEST_F(ImageTest, TortureAndBarrierVerifyPassOverARestoredHeap) {
  for (uint64_t seed = 1; seed <= 15; seed++) {
    ImgWorld src(1);
    Model m;
    build(src, m, seed, 8 + static_cast<int>(seed * 3), 4);
    Taken t;
    ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
    WorldConfig c;
    c.torture = 1;
    c.verify = GRHEAP_VERIFY_ABORT;
    ImgWorld dst(src.box.words.size(), c);
    GRHEAP_RestoreEnv env = env_with();
    ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
    uint64_t violations = 99;
    ASSERT_EQ(grheap_verify(dst.w.heap, &violations), GRHEAP_OK);
    EXPECT_EQ(violations, 0u) << "every restored slot is recorded as stored";
    // Collections at every allocation, with the restored objects the only
    // thing keeping each other alive.
    for (int i = 0; i < 20; i++) {
      void * junk;
      ASSERT_EQ(grheap_alloc(dst.w.heap, &kLeaf, &junk), GRHEAP_OK);
    }
    EXPECT_EQ(compare(dst, m, true), "") << "seed " << seed;
    EXPECT_EQ(dst.w.stats().live_objects, reach_count(m));
    EXPECT_GT(dst.w.stats().torture_collections, 0u);
    // A store through the barrier after the restore verifies too.
    for (uint64_t word : dst.box.words) {
      if (word != 0) {
        auto * n = reinterpret_cast<Node *>(word);
        ASSERT_EQ(grheap_store(dst.w.heap, n, &n->b, nullptr), GRHEAP_OK);
        break;
      }
    }
    ASSERT_EQ(grheap_verify(dst.w.heap, &violations), GRHEAP_OK);
    EXPECT_EQ(violations, 0u);
  }
}

TEST_F(ImageTest, BarrierVerifyWouldHaveFoundARestoreThatForgotTheShadow) {
  // The control for the test above: a slot written without the barrier shows
  // as a violation, so "0 violations" over a restored heap means something.
  ImgWorld src(1);
  Model m;
  build(src, m, 3, 10, 2);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  WorldConfig c;
  c.torture = 0;
  c.verify = GRHEAP_VERIFY_COUNT;
  ImgWorld dst(src.box.words.size(), c);
  GRHEAP_RestoreEnv env = env_with();
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  Node * victim = nullptr;
  for (uint64_t word : dst.box.words) {
    if (word != 0) {
      victim = reinterpret_cast<Node *>(word);
      break;
    }
  }
  ASSERT_NE(victim, nullptr);
  victim->a = victim; // not through grheap_store
  uint64_t violations = 0;
  ASSERT_EQ(grheap_verify(dst.w.heap, &violations), GRHEAP_OK);
  EXPECT_GT(violations, 0u);
}

/* ---- host pointers, by name ------------------------------------------------------------ */

TEST_F(ImageTest, ATypesHooksNameAHostPointerAndTheDestinationResolvesItByName) {
  ImgWorld src(1);
  void * p = nullptr;
  Named * prev = nullptr;
  for (int i = 0; i < 6; i++) {
    ASSERT_EQ(grheap_alloc(src.w.heap, &kNamed, &p), GRHEAP_OK);
    auto * n = static_cast<Named *>(p);
    n->label = kA[i % 3];
    n->n = static_cast<uint64_t>(i);
    if (prev != nullptr) {
      ASSERT_EQ(grheap_store(src.w.heap, n, &n->next, prev), GRHEAP_OK);
    }
    prev = n;
    src.set(0, n);
  }
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);

  Labels dest_labels{{kB[0], kB[1], kB[2]}}; // a table in another order
  ImgWorld dst(1);
  GRHEAP_RestoreEnv env = env_with(&dest_labels);
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  auto * n = reinterpret_cast<Named *>(dst.box.words[0]);
  for (int i = 5; i >= 0; i--) {
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->n, static_cast<uint64_t>(i));
    EXPECT_STREQ(n->label, kA[i % 3]) << "the same name";
    bool in_table = false;
    for (const char * name : kB) {
      in_table = in_table || n->label == name;
    }
    EXPECT_TRUE(in_table) << "and the destination's own pointer for it";
    n = static_cast<Named *>(n->next);
  }
  EXPECT_EQ(n, nullptr);
}

TEST_F(ImageTest, ANameThatDoesNotResolveRefusesTheRestoreAndLeavesTheHeapEmpty) {
  ImgWorld src(1);
  void * p = nullptr;
  ASSERT_EQ(grheap_alloc(src.w.heap, &kNamed, &p), GRHEAP_OK);
  src.set(0, p);
  static_cast<Named *>(p)->label = "purple";
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  Labels labels{{kA[0], kA[1], kA[2]}};
  ImgWorld dst(1);
  GRHEAP_RestoreEnv env = env_with(&labels);
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
  EXPECT_EQ(dst.w.stats().live_objects, 0u);
  EXPECT_EQ(dst.box.words[0], 0u);
  EXPECT_EQ(g_named_destroyed, 1u) << "the object the restore made is destroyed once";
  // Still a fresh heap: a good image goes in.
  ImgWorld other(1);
  ASSERT_EQ(grheap_alloc(other.w.heap, &kNamed, &p), GRHEAP_OK);
  other.set(0, p);
  static_cast<Named *>(p)->label = kA[1];
  Taken t2;
  ASSERT_EQ(grheap_image_take(other.w.heap, nullptr, &t2.image), GRHEAP_OK);
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t2.image, &env), GRHEAP_OK);
}

TEST_F(ImageTest, ARestoreHookThatFailsPartWayRemovesEveryObjectTheRestoreMade) {
  ImgWorld src(1);
  void * p = nullptr;
  Named * prev = nullptr;
  for (int i = 0; i < 6; i++) {
    ASSERT_EQ(grheap_alloc(src.w.heap, &kFlaky, &p), GRHEAP_OK);
    auto * n = static_cast<Named *>(p);
    n->n = static_cast<uint64_t>(i);
    if (prev != nullptr) {
      ASSERT_EQ(grheap_store(src.w.heap, n, &n->next, prev), GRHEAP_OK);
    }
    prev = n;
    src.set(0, n);
  }
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  ImgWorld dst(1);
  GRHEAP_RestoreEnv env = env_with();
  g_named_destroyed = 0;
  g_flaky_seen.clear();
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
  EXPECT_EQ(dst.w.stats().live_objects, 0u);
  EXPECT_EQ(dst.w.stats().ids_live, 0u);
  EXPECT_EQ(g_named_destroyed, 6u) << "each object destroyed once, none twice";
  // Objects whose restore ran, objects whose restore never did (the failure
  // was at the fourth) and the one that failed are all destroyed holding what
  // the snapshot left: a cleared host pointer, never one from the image.
  ASSERT_EQ(g_flaky_seen.size(), 6u);
  for (const char * label : g_flaky_seen) {
    EXPECT_EQ(label, nullptr);
  }
  EXPECT_EQ(dst.box.words[0], 0u);
}

TEST_F(ImageTest, AReaderThatRunsPastTheBlobOrFindsNoTerminatorIsCorruptAndTheHeapStaysEmpty) {
  ImgWorld src(1);
  void * p = nullptr;
  ASSERT_EQ(grheap_alloc(src.w.heap, &kReader, &p), GRHEAP_OK);
  src.set(0, p);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  ImgWorld dst(1);
  GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT([](void *, const char * name) -> const GRHEAP_Type * {
                             return std::strcmp(name, "reader") == 0 ? &kReader
                                                                     : nullptr;
                           },
      nullptr);
  g_reader_seen[0] = g_reader_seen[1] = g_reader_seen[2] = GRHEAP_OK;
  EXPECT_NE(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  EXPECT_EQ(g_reader_seen[0], GRHEAP_ERR_CORRUPT); // no terminator
  EXPECT_EQ(g_reader_seen[1], GRHEAP_ERR_CORRUPT); // past the end, and stays failed
  EXPECT_EQ(g_reader_seen[2], GRHEAP_ERR_CORRUPT);
  EXPECT_EQ(dst.w.stats().live_objects, 0u);
}

TEST_F(ImageTest, AHookThatRefusesToSnapshotRefusesTheImageAndChangesNothing) {
  static const GRHEAP_Type refuses = GRHEAP_TYPE_INIT("refuses", sizeof(Named), nullptr, nullptr,
      nullptr,
      [](GRCORE_Context *, void *, GRHEAP_ImageWriter *) -> GRHEAP_Result {
        return GRHEAP_ERR_INVALID;
      },
      nullptr);
  ImgWorld src(1);
  void * p = nullptr;
  ASSERT_EQ(grheap_alloc(src.w.heap, &refuses, &p), GRHEAP_OK);
  src.set(0, p);
  GRHEAP_Image * image = nullptr;
  EXPECT_EQ(grheap_image_take(src.w.heap, nullptr, &image), GRHEAP_ERR_INVALID);
  EXPECT_EQ(image, nullptr);
  EXPECT_EQ(src.w.stats().live_objects, 1u);
}

/* ---- refusing to take ---------------------------------------------------------------------- */

TEST_F(ImageTest, EverythingTheImageCannotCarryRefusesATakeAndChangesNothing) {
  GRHEAP_Image * image = nullptr;
  {
    ImgWorld w(1);
    void * slot = nullptr;
    ASSERT_EQ(grheap_root_add(w.w.heap, &slot), GRHEAP_OK);
    EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, &image), GRHEAP_ERR_INVALID)
        << "a C root";
    ASSERT_EQ(grheap_root_remove(w.w.heap, &slot), GRHEAP_OK);
    EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, &image), GRHEAP_OK);
    grheap_image_release(image);
    image = nullptr;
  }
  {
    ImgWorld w(1);
    Node * n = new_node(w.w.heap, 1, &kPlainNode);
    w.set(0, n);
    GRHEAP_Handle * h;
    ASSERT_EQ(grheap_handle_create(w.w.heap, n, &h), GRHEAP_OK);
    EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, &image), GRHEAP_ERR_INVALID) << "a handle";
    ASSERT_EQ(grheap_handle_destroy(h), GRHEAP_OK);
    ASSERT_EQ(grheap_pin(w.w.heap, n), GRHEAP_OK);
    EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, &image), GRHEAP_ERR_INVALID) << "a pin";
    ASSERT_EQ(grheap_unpin(w.w.heap, n), GRHEAP_OK);
    EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, &image), GRHEAP_OK);
    grheap_image_release(image);
    image = nullptr;
  }
  {
    ImgWorld w(1);
    void * p;
    ASSERT_EQ(grheap_alloc(w.w.heap, &kWCell, &p), GRHEAP_OK);
    w.set(0, p);
    EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, &image), GRHEAP_ERR_INVALID)
        << "a weak cell";
    EXPECT_EQ(image, nullptr);
  }
  {
    ImgWorld w(1);
    RangeBox range;
    ASSERT_EQ(grcore_context_add_root_source(w.w.ctx, &kRangeSource, &range), GRCORE_OK);
    EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, &image), GRHEAP_ERR_INVALID)
        << "a conservative range";
    range.empty = true;
    EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, &image), GRHEAP_OK)
        << "an empty range is no range";
    grheap_image_release(image);
    image = nullptr;
  }
  {
    ImgWorld w(1);
    void * p;
    ASSERT_EQ(grheap_alloc(w.w.heap, &kNameless, &p), GRHEAP_OK);
    w.set(0, p);
    EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, &image), GRHEAP_ERR_INVALID)
        << "a type with no name";
  }
  {
    ImgWorld w(2);
    void * p;
    ASSERT_EQ(grheap_alloc(w.w.heap, &kTwinOne, &p), GRHEAP_OK);
    w.set(0, p);
    ASSERT_EQ(grheap_alloc(w.w.heap, &kTwinTwo, &p), GRHEAP_OK);
    w.set(1, p);
    EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, &image), GRHEAP_ERR_INVALID)
        << "two types, one name";
  }
  {
    ImgWorld w(1);
    void * p;
    ASSERT_EQ(grheap_alloc(w.w.heap, &kOutside, &p), GRHEAP_OK);
    w.set(0, p);
    EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, &image), GRHEAP_ERR_INVALID)
        << "a slot outside the payload";
  }
  {
    ImgWorld w(1);
    GRHEAP_Result r = GRHEAP_OK;
    std::thread([&] { r = grheap_image_take(w.w.heap, nullptr, &image); }).join();
    EXPECT_EQ(r, GRHEAP_ERR_INVALID) << "not the owner";
  }
  EXPECT_EQ(image, nullptr);
  GRHEAP_Image * unused;
  EXPECT_EQ(grheap_image_take(nullptr, nullptr, &unused), GRHEAP_ERR_INVALID);
  ImgWorld w(1);
  EXPECT_EQ(grheap_image_take(w.w.heap, nullptr, nullptr), GRHEAP_ERR_INVALID);
}

TEST_F(ImageTest, AStaleRootWordIsNotCarriedAndTheImageStillTakes) {
  ImgWorld w(2);
  Node * n = new_node(w.w.heap, 1, &kPlainNode);
  w.set(0, n);
  w.box.words[1] = addr(n) + 8; // an interior address: names no object
  Taken t;
  ASSERT_EQ(grheap_image_take(w.w.heap, nullptr, &t.image), GRHEAP_OK);
  EXPECT_EQ(grheap_image_object_count(t.image), 1u);
  ImgWorld dst(2);
  GRHEAP_RestoreEnv env = env_with();
  ASSERT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
  EXPECT_NE(dst.box.words[0], 0u);
  EXPECT_EQ(dst.box.words[1], 0u);
}

/* ---- refusing to instantiate ---------------------------------------------------------------- */

TEST_F(ImageTest, InstantiateIsRefusedIntoAHeapThatIsNotEmptyOrHasIds) {
  ImgWorld src(1);
  Node * n = new_node(src.w.heap, 1, &kPlainNode);
  src.set(0, n);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  GRHEAP_RestoreEnv env = env_with();
  {
    ImgWorld dst(1);
    Node * held = new_node(dst.w.heap, 5, &kPlainNode);
    dst.set(0, held);
    EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
    EXPECT_EQ(dst.w.stats().live_objects, 1u);
    EXPECT_EQ(dst.box.words[0], addr(held));
  }
  {
    ImgWorld dst(1);
    uint64_t id;
    Node * held = new_node(dst.w.heap, 5, &kPlainNode);
    dst.set(0, held);
    ASSERT_EQ(grheap_id_of(dst.w.heap, held, &id), GRHEAP_OK);
    dst.box.words[0] = 0;
    ASSERT_EQ(grheap_collect(dst.w.heap), GRHEAP_OK);
    EXPECT_EQ(dst.w.stats().live_objects, 0u);
    EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID)
        << "ids were issued: the numbering is not fresh";
  }
}

TEST_F(ImageTest, ATypeTheDestinationDoesNotKnowAndABadArgumentRefuseTheRestore) {
  ImgWorld src(1);
  Node * n = new_node(src.w.heap, 1, &kPlainNode);
  src.set(0, n);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  ImgWorld dst(1);
  GRHEAP_RestoreEnv none = GRHEAP_RESTORE_ENV_INIT(resolve_nothing, nullptr);
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &none), GRHEAP_ERR_INVALID);
  GRHEAP_RestoreEnv unset = GRHEAP_RESTORE_ENV_INIT(nullptr, nullptr);
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &unset), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, nullptr), GRHEAP_ERR_INVALID);
  GRHEAP_RestoreEnv env = env_with();
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, nullptr, &env), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_image_instantiate(nullptr, t.image, &env), GRHEAP_ERR_INVALID);
  EXPECT_EQ(dst.w.stats().live_objects, 0u);
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK);
}

TEST_F(ImageTest, ADestinationTypeWithAWeakSpecIsRefused) {
  ImgWorld src(1);
  Node * n = new_node(src.w.heap, 1, &kPlainNode);
  src.set(0, n);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  ImgWorld dst(1);
  static const GRHEAP_Type weak_plain = GRHEAP_TYPE_INIT("plain", sizeof(WCell), nullptr, nullptr,
      &kWCellSpec, nullptr, nullptr);
  GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT([](void *, const char *) -> const GRHEAP_Type * {
                             return &weak_plain;
                           },
      nullptr);
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
  EXPECT_EQ(dst.w.stats().live_objects, 0u);
}

TEST_F(ImageTest, ADifferentValueCodecIsRefusedBeforeAnythingChanges) {
  ImgWorld src(1);
  Node * n = new_node(src.w.heap, 1, &kPlainNode);
  src.set(0, n);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  WorldConfig c;
  c.codec_set = true;
  c.codec = {0xF, 0, ~UINT64_C(0xF), 0, 0}; // a tagged codec
  ImgWorld dst(1, c);
  GRHEAP_RestoreEnv env = env_with();
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
  EXPECT_EQ(dst.w.stats().live_objects, 0u);
}

TEST_F(ImageTest, ADifferentRootLayoutIsRefusedAndTheObjectsAlreadyMadeAreRemoved) {
  ImgWorld src(3);
  Model m;
  build(src, m, 11, 12, 3);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  GRHEAP_RestoreEnv env = env_with();
  {
    ImgWorld dst(src.box.words.size() + 1); // one slot too many
    EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
    EXPECT_EQ(dst.w.stats().live_objects, 0u) << "APPLY made objects; ABANDON removed them";
    EXPECT_EQ(dst.w.stats().ids_live, 0u);
    EXPECT_EQ(compare(dst, Model{{}, {}, std::vector<int>(dst.box.words.size(), -1)}, true), "");
  }
  {
    ImgWorld dst(src.box.words.size());
    RangeBox range;
    ASSERT_EQ(grcore_context_add_root_source(dst.w.ctx, &kRangeSource, &range), GRCORE_OK);
    EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID)
        << "a range at the destination that the source did not have";
    EXPECT_EQ(dst.w.stats().live_objects, 0u);
  }
  {
    ImgWorld dst(src.box.words.size());
    EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK)
        << "the right layout takes it";
    EXPECT_EQ(compare(dst, m, false), "");
  }
}

TEST_F(ImageTest, AnImageNeedsTheTypesRestoreHookWhenItCarriedExtraBytes) {
  ImgWorld src(1);
  void * p = nullptr;
  ASSERT_EQ(grheap_alloc(src.w.heap, &kNamed, &p), GRHEAP_OK);
  src.set(0, p);
  static_cast<Named *>(p)->label = "red";
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  ImgWorld dst(1);
  // A destination whose "named" has no restore hook cannot take the extra bytes.
  static const GRHEAP_Type deaf = GRHEAP_TYPE_INIT("named", sizeof(Named), named_trace, nullptr,
      nullptr, nullptr, nullptr);
  GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT([](void *, const char *) -> const GRHEAP_Type * {
                             return &deaf;
                           },
      nullptr);
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_INVALID);
  EXPECT_EQ(dst.w.stats().live_objects, 0u);
}

/* ---- budgets and allocation failure ---------------------------------------------------------- */

TEST_F(ImageTest, ADestinationBudgetSmallerThanTheImageFailsWithLimitAndLeavesItEmpty) {
  ImgWorld src(1);
  Model m;
  build(src, m, 5, 4000, 3);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  ASSERT_GT(grheap_image_object_count(t.image), 1500u) << "more than a block's worth";
  WorldConfig c;
  c.memory = 66000; // one 64 KiB block and a little, then nothing
  c.reserve = 0;
  ImgWorld dst(src.box.words.size(), c);
  GRHEAP_RestoreEnv env = env_with();
  EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_ERR_LIMIT);
  EXPECT_EQ(dst.w.stats().live_objects, 0u);
  EXPECT_EQ(dst.box.words[0], 0u);
}

TEST_F(ImageTest, EveryAllocationFailureDuringTakeIsACleanErrorWithNothingLeaked) {
  ImgWorld src(1);
  Model m;
  build(src, m, 9, 25, 3);
  long total = 0;
  {
    TrackingAllocator probe;
    GRHEAP_Image * image = nullptr;
    ASSERT_EQ(grheap_image_take(src.w.heap, probe.get(), &image), GRHEAP_OK);
    grheap_image_release(image);
    total = probe.calls;
  }
  ASSERT_GT(total, 10);
  for (long n = 1; n <= total; n++) {
    TrackingAllocator mem;
    mem.fail_at = n;
    GRHEAP_Image * image = nullptr;
    EXPECT_EQ(grheap_image_take(src.w.heap, mem.get(), &image), GRHEAP_ERR_OOM)
        << "failing allocation " << n;
    EXPECT_EQ(image, nullptr);
    EXPECT_EQ(mem.live, 0) << "leak at allocation " << n;
  }
}

TEST_F(ImageTest, EveryAllocationFailureDuringInstantiateLeavesTheHeapEmptyAndLeaksNothing) {
  ImgWorld src(1);
  Model m;
  build(src, m, 13, 30, 3);
  uint64_t first_id = 0;
  ASSERT_EQ(grheap_id_of(src.w.heap, reinterpret_cast<void *>(src.box.words[0]),
                &first_id),
      GRHEAP_OK);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  GRHEAP_RestoreEnv env = env_with();
  long total = 0;
  {
    ImgWorld probe(src.box.words.size());
    long before = probe.w.allocator.calls;
    ASSERT_EQ(grheap_image_instantiate(probe.w.heap, t.image, &env), GRHEAP_OK);
    total = probe.w.allocator.calls - before;
  }
  ASSERT_GT(total, 3);
  for (long k = 1; k <= total; k++) {
    ImgWorld dst(src.box.words.size());
    dst.w.allocator.fail_at = dst.w.allocator.calls + k;
    GRHEAP_Result r = grheap_image_instantiate(dst.w.heap, t.image, &env);
    dst.w.allocator.fail_at = 0;
    if (r == GRHEAP_OK) {
      // Only an instrument's allocation may fail without failing the restore:
      // barrier-verify's shadow table degrades (the heap stops trusting it)
      // rather than refuse. The restore is then complete and correct.
      const char * verify = std::getenv("GRHEAP_VERIFY");
      EXPECT_TRUE(verify != nullptr && *verify != '\0') << "allocation " << k;
      EXPECT_EQ(compare(dst, m, false), "");
      continue;
    }
    EXPECT_TRUE(r == GRHEAP_ERR_OOM || r == GRHEAP_ERR_LIMIT) << "allocation " << k << ": " << r;
    EXPECT_EQ(dst.w.stats().live_objects, 0u) << "allocation " << k;
    EXPECT_EQ(dst.w.stats().ids_live, 0u);
    // Still fresh: the same restore now succeeds.
    EXPECT_EQ(grheap_image_instantiate(dst.w.heap, t.image, &env), GRHEAP_OK) << "allocation " << k;
    EXPECT_EQ(compare(dst, m, false), "");
  }
}

/* ---- the image object ------------------------------------------------------------------------- */

TEST_F(ImageTest, AnImageIsReferenceCountedOutlivesItsHeapAndRestoresManyTimes) {
  GRHEAP_Image * image = nullptr;
  Model m;
  {
    ImgWorld src(1);
    build(src, m, 21, 20, 3);
    ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &image), GRHEAP_OK);
  }
  EXPECT_EQ(grheap_image_retain(image), image);
  EXPECT_EQ(grheap_image_refcount(image), 2u);
  grheap_image_release(image);
  GRHEAP_RestoreEnv env = env_with();
  for (int i = 0; i < 3; i++) {
    ImgWorld dst(m.root_of.size());
    ASSERT_EQ(grheap_image_instantiate(dst.w.heap, image, &env), GRHEAP_OK);
    EXPECT_EQ(compare(dst, m, true), "");
  }
  grheap_image_release(image);
  EXPECT_EQ(grheap_image_retain(nullptr), nullptr);
  grheap_image_release(nullptr);
  EXPECT_EQ(grheap_image_size(nullptr), 0u);
  EXPECT_EQ(grheap_image_object_count(nullptr), 0u);
  EXPECT_EQ(grheap_image_root_count(nullptr), 0u);
  EXPECT_EQ(grheap_image_bytes(nullptr, nullptr), nullptr);
}

TEST_F(ImageTest, OneImageRestoredConcurrentlyIntoHeapsOnManyThreads) {
  ImgWorld src(1);
  Model m;
  build(src, m, 33, 60, 4);
  Taken t;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &t.image), GRHEAP_OK);
  std::atomic<int> good{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; i++) {
    threads.emplace_back([&] {
      for (int round = 0; round < 20; round++) {
        grheap_image_retain(t.image);
        {
          ImgWorld dst(m.root_of.size());
          GRHEAP_RestoreEnv env = env_with();
          if (grheap_image_instantiate(dst.w.heap, t.image, &env) == GRHEAP_OK &&
              compare(dst, m, true).empty()) {
            good++;
          }
        }
        grheap_image_release(t.image);
      }
    });
  }
  for (auto & th : threads) {
    th.join();
  }
  EXPECT_EQ(good.load(), 8 * 20);
  EXPECT_EQ(grheap_image_refcount(t.image), 1u);
}

/* ---- the heap's key hooks, inside a context snapshot --------------------------------------------- */

TEST_F(ImageTest, TheHeapKeyCarriesTheImageInsideAContextSnapshot) {
  ImgWorld src(1);
  Model m;
  build(src, m, 17, 25, 3);
  GRCORE_Snapshot * snap = nullptr;
  ASSERT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &snap), GRCORE_OK);
  ASSERT_EQ(grcore_snapshot_blob_count(snap), 1u);
  EXPECT_STREQ(grcore_snapshot_blob_name(snap, 0), GRHEAP_SNAPSHOT_KEY_NAME);

  GRHEAP_RestoreEnv env = env_with();
  GRCORE_RestoreEnv renv = GRCORE_RESTORE_ENV_INIT(nullptr, nullptr, nullptr, nullptr, nullptr);
  renv.user = &env;
  renv.lookup = [](void * u, const char * name) -> void * {
    return std::strcmp(name, GRHEAP_SNAPSHOT_KEY_NAME) == 0 ? u : nullptr;
  };
  {
    ImgWorld dst(src.box.words.size());
    ASSERT_EQ(grcore_context_restore(dst.w.ctx, snap, &renv), GRCORE_OK);
    EXPECT_EQ(compare(dst, m, true), "");
    // The snapshot restores again, into another heap, with ids and all.
    ImgWorld again(src.box.words.size());
    ASSERT_EQ(grcore_context_restore(again.w.ctx, snap, &renv), GRCORE_OK);
    EXPECT_EQ(compare(again, m, true), "");
  }
  {
    // A destination whose root layout differs fails at PREPARE and is fresh.
    ImgWorld dst(src.box.words.size() + 2);
    EXPECT_EQ(grcore_context_restore(dst.w.ctx, snap, &renv), GRCORE_ERR_INVALID);
    EXPECT_EQ(dst.w.stats().live_objects, 0u);
  }
  {
    // A different codec is refused in CHECK, before any object is made.
    WorldConfig c;
    c.codec_set = true;
    c.codec = {0xF, 0, ~UINT64_C(0xF), 0, 0};
    ImgWorld dst(src.box.words.size(), c);
    EXPECT_EQ(grcore_context_restore(dst.w.ctx, snap, &renv), GRCORE_ERR_INVALID);
    EXPECT_EQ(dst.w.stats().allocations, 0u);
  }
  {
    // No environment: nothing to resolve the types with.
    ImgWorld dst(src.box.words.size());
    EXPECT_EQ(grcore_context_restore(dst.w.ctx, snap, nullptr), GRCORE_ERR_INVALID);
    EXPECT_EQ(dst.w.stats().allocations, 0u);
  }
  grcore_snapshot_release(snap);
}

TEST_F(ImageTest, ATakeThroughTheKeyIsRefusedForWhatTheImageCannotCarry) {
  ImgWorld src(1);
  void * slot = nullptr;
  ASSERT_EQ(grheap_root_add(src.w.heap, &slot), GRHEAP_OK);
  GRCORE_Snapshot * snap = nullptr;
  EXPECT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &snap), GRCORE_ERR_INVALID);
  EXPECT_EQ(snap, nullptr);
  ASSERT_EQ(grheap_root_remove(src.w.heap, &slot), GRHEAP_OK);
  EXPECT_EQ(grcore_context_snapshot(src.w.ctx, nullptr, &snap), GRCORE_OK);
  grcore_snapshot_release(snap);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
