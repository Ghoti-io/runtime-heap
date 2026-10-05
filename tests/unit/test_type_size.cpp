/**
 * @file
 *
 * A type descriptor states its own size (type.h), so the struct can grow at
 * the end. The cases are those of runtime-core's test_key_size.cpp, for the
 * members a type gained after the first generation: `weak`, and the image
 * hooks `snapshot` and `restore`. (The leading member is `struct_size`,
 * because `size` is the payload's.)
 *
 * Every case is a type as an older or a newer header would have built it, and
 * each has a control: the same type at its full size, which must behave the
 * other way, so that a test that passes because the member was never wired
 * cannot pass. The members that must not be read are armed: a weak spec whose
 * slot would be cleared and whose `cleared` hook counts, and image hooks that
 * count. The older-layout copy is exactly as large as its size says, on the
 * heap, so a read past the end is a finding under ASan or Valgrind and not a
 * silent zero.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <ghoti.io/runtime-core/b/roots.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

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

struct BoxWorld {
  RootBox box;
  HeapWorld w;
  BoxWorld() : w(steady()) {
    box.words.assign(1, 0);
    EXPECT_EQ(grcore_context_add_root_source(w.ctx, &kBoxSource, &box), GRCORE_OK);
  }
};

/* ---- the payloads and the armed members ----------------------------------- */

struct Cell {
  void * target; // the weak slot, in a type that has a weak spec
  uint64_t id;
};

int g_cleared = 0;
int g_snapshots = 0;
int g_restores = 0;
int g_destroyed = 0;

void cell_cleared(void *) { g_cleared++; }
void cell_destroy(void *) { g_destroyed++; }
void cell_trace(GRHEAP_Tracer * t, void * payload) {
  grheap_trace_slot(t, &static_cast<Cell *>(payload)->target);
}

const GRHEAP_WeakSpec kSpec = {
    GRHEAP_WEAK_CELL, offsetof(Cell, target), 0, 0, false, cell_cleared};

GRHEAP_Result cell_snapshot(GRCORE_Context *, void *, GRHEAP_ImageWriter * w) {
  g_snapshots++;
  return grheap_image_writer_write(w, "x", 1);
}
GRHEAP_Result cell_restore(
    GRCORE_Context *, void *, GRHEAP_ImageReader * r, void *) {
  g_restores++;
  char c;
  return grheap_image_reader_read(r, &c, 1);
}

void reset_counts() {
  g_cleared = g_snapshots = g_restores = g_destroyed = 0;
}

// The type as the first generation had it: everything through `destroy`, and
// nothing after. A copy of the layout, so the bytes after it are not the
// test's.
struct OldType {
  size_t struct_size;
  const char * name;
  size_t size;
  GRHEAP_TraceFn trace;
  GRHEAP_DestroyFn destroy;
};
static_assert(sizeof(OldType) == GRHEAP_TYPE_MIN_SIZE,
    "the old layout is the minimum layout");
static_assert(offsetof(OldType, destroy) == offsetof(GRHEAP_Type, destroy),
    "the old layout is a prefix of the current one");
static_assert(offsetof(OldType, size) == offsetof(GRHEAP_Type, size), "prefix");

const GRHEAP_Type * old_type_on_heap(const char * name) {
  auto * t = static_cast<OldType *>(std::malloc(sizeof(OldType)));
  t->struct_size = sizeof(OldType);
  t->name = name;
  t->size = sizeof(Cell);
  t->trace = cell_trace;
  t->destroy = cell_destroy;
  return reinterpret_cast<const GRHEAP_Type *>(t);
}

// A weak cell whose every trailing member is armed: the control, at full size
// (it has a weak spec, so it cannot be in an image: `hooked` is the one that
// can).
GRHEAP_Type weak_type(const char * name) {
  return GRHEAP_TYPE_INIT(name, sizeof(Cell), nullptr, cell_destroy, &kSpec,
      nullptr, nullptr);
}
GRHEAP_Type hooked_type(const char * name) {
  return GRHEAP_TYPE_INIT(name, sizeof(Cell), nullptr, cell_destroy, nullptr,
      cell_snapshot, cell_restore);
}

Cell * alloc_cell(HeapWorld & w, const GRHEAP_Type * type) {
  void * p = nullptr;
  EXPECT_EQ(grheap_alloc(w.heap, type, &p), GRHEAP_OK);
  return static_cast<Cell *>(p);
}

void * alloc_leaf(HeapWorld & w) {
  void * p = nullptr;
  EXPECT_EQ(grheap_alloc(w.heap, &kLeaf, &p), GRHEAP_OK);
  return p;
}

// The resolver the instantiation uses: the one type every object has.
const GRHEAP_Type * g_resolved = nullptr;
const GRHEAP_Type * resolve_it(void *, const char * name) {
  return g_resolved != nullptr && std::strcmp(g_resolved->name, name) == 0
      ? g_resolved
      : nullptr;
}

} // namespace

TEST(TypeSize, TheInitialiserWritesTheSizeOfTheStructItWasCompiledAgainst) {
  static const GRHEAP_Type t =
      GRHEAP_TYPE_INIT("t", 8, nullptr, nullptr, nullptr, nullptr, nullptr);
  EXPECT_EQ(t.struct_size, sizeof(GRHEAP_Type));
  EXPECT_EQ(t.size, 8u) << "the payload's size is its own member";
  EXPECT_GE(t.struct_size, GRHEAP_TYPE_MIN_SIZE);
  EXPECT_TRUE(grheap_type_valid(&t));
  EXPECT_FALSE(grheap_type_valid(nullptr));
}

TEST(TypeSize, AnOlderShorterTypeAllocatesTracesDestroysAndImagesWithItsAbsentMembersNeverRead) {
  reset_counts();
  const GRHEAP_Type * old = old_type_on_heap("old");
  g_resolved = old;
  {
    BoxWorld src;
    Cell * a = alloc_cell(src.w, old);
    src.box.words[0] = addr(a);
    Cell * b = alloc_cell(src.w, old);
    ASSERT_EQ(grheap_store(src.w.heap, a, &a->target, b), GRHEAP_OK);
    alloc_cell(src.w, old); // unreachable
    // Marking reads `trace`, the sweep reads `destroy`, and neither looks for
    // `weak`, `snapshot` or `restore`, which this type does not have.
    ASSERT_EQ(grheap_collect(src.w.heap), GRHEAP_OK);
    EXPECT_EQ(g_destroyed, 1) << "the unreachable one";
    GRHEAP_Image * image = nullptr;
    ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &image), GRHEAP_OK);
    EXPECT_EQ(grheap_image_object_count(image), 2u);
    {
      BoxWorld dst;
      GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT(resolve_it, nullptr);
      ASSERT_EQ(grheap_image_instantiate(dst.w.heap, image, &env), GRHEAP_OK);
      EXPECT_EQ(dst.w.stats().live_objects, 2u);
    }
    grheap_image_release(image);
  }
  g_resolved = nullptr;
  std::free(const_cast<GRHEAP_Type *>(old));
}

TEST(TypeSize, AWeakSpecBeyondTheStatedSizeIsNotReadAndAtFullSizeIs) {
  // The plain-run form of the test above: the spec is real and armed, and
  // only `struct_size` keeps it unread.
  for (int full = 0; full < 2; full++) {
    reset_counts();
    GRHEAP_Type type = weak_type("weak");
    if (!full) {
      type.struct_size = offsetof(GRHEAP_Type, weak);
    }
    BoxWorld w;
    Cell * cell = alloc_cell(w.w, &type);
    w.box.words[0] = addr(cell);
    void * target = alloc_leaf(w.w);
    ASSERT_EQ(grheap_store(w.w.heap, cell, &cell->target, target), GRHEAP_OK);
    ASSERT_EQ(grheap_collect(w.w.heap), GRHEAP_OK);
    if (full) {
      EXPECT_EQ(cell->target, nullptr) << "the weak slot was cleared";
      EXPECT_EQ(g_cleared, 1);
    } else {
      EXPECT_NE(cell->target, nullptr) << "no weak spec was read";
      EXPECT_EQ(g_cleared, 0);
    }
    cell->target = nullptr; // the target is gone; nothing may follow it
  }
}

TEST(TypeSize, EachTrailingMemberIsReadOnlyWhereTheSizeReachesItsEnd) {
  struct Cut {
    size_t size;
    bool snapshots;
    bool restores;
  };
  const Cut cuts[] = {
      {offsetof(GRHEAP_Type, weak), false, false},
      {offsetof(GRHEAP_Type, snapshot), false, false},
      {offsetof(GRHEAP_Type, snapshot) + sizeof(void *), true, false},
      {offsetof(GRHEAP_Type, restore) + sizeof(void *), true, true},
  };
  for (const Cut & cut : cuts) {
    reset_counts();
    GRHEAP_Type type = hooked_type("hooked");
    type.struct_size = cut.size;
    g_resolved = &type;
    BoxWorld src;
    Cell * c = alloc_cell(src.w, &type);
    src.box.words[0] = addr(c);
    GRHEAP_Image * image = nullptr;
    ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &image), GRHEAP_OK)
        << "size " << cut.size;
    EXPECT_EQ(g_snapshots, cut.snapshots ? 1 : 0) << "size " << cut.size;
    BoxWorld dst;
    GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT(resolve_it, nullptr);
    GRHEAP_Result r = grheap_image_instantiate(dst.w.heap, image, &env);
    if (cut.snapshots && !cut.restores) {
      // The image holds extra bytes and the type has no `restore` to take
      // them: the instantiation refuses, as it does for any such type.
      EXPECT_EQ(r, GRHEAP_ERR_INVALID) << "size " << cut.size;
    } else {
      EXPECT_EQ(r, GRHEAP_OK) << "size " << cut.size;
    }
    EXPECT_EQ(g_restores, cut.restores ? 1 : 0) << "size " << cut.size;
    grheap_image_release(image);
    g_resolved = nullptr;
  }
}

TEST(TypeSize, ASizeBelowTheMinimumOrOffAlignmentIsRefusedNotRead) {
  const size_t bad[] = {0, 1, GRHEAP_TYPE_MIN_SIZE - sizeof(void *),
      GRHEAP_TYPE_MIN_SIZE - 1, GRHEAP_TYPE_MIN_SIZE + 1,
      sizeof(GRHEAP_Type) + 1, sizeof(GRHEAP_Type) + sizeof(void *) / 2};
  for (size_t size : bad) {
    GRHEAP_Type type = hooked_type("bad");
    type.struct_size = size;
    EXPECT_FALSE(grheap_type_valid(&type)) << "size " << size;
    HeapWorld w(steady());
    void * out = reinterpret_cast<void *>(1);
    EXPECT_EQ(grheap_alloc(w.heap, &type, &out), GRHEAP_ERR_INVALID)
        << "size " << size;
    EXPECT_EQ(grheap_alloc_sized(w.heap, &type, 16, &out), GRHEAP_ERR_INVALID)
        << "size " << size;
    EXPECT_EQ(out, reinterpret_cast<void *>(1)) << "written only on success";
    EXPECT_EQ(w.stats().allocations, 0u);
  }
  // Controls: the smallest size and the full size are accepted, so the list
  // above is refused for its size and not for anything else about it.
  for (size_t size : {GRHEAP_TYPE_MIN_SIZE, sizeof(GRHEAP_Type)}) {
    GRHEAP_Type type = hooked_type("ok");
    type.struct_size = size;
    HeapWorld w(steady());
    void * out = nullptr;
    EXPECT_EQ(grheap_alloc(w.heap, &type, &out), GRHEAP_OK) << "size " << size;
  }
}

TEST(TypeSize, AResolverThatReturnsAnInvalidTypeRefusesTheInstantiation) {
  // A type with no image hooks, so that the image holds no extra bytes and the
  // only thing that can refuse the instantiation is the type's own size.
  GRHEAP_Type good = GRHEAP_TYPE_INIT("resolved", sizeof(Cell), nullptr,
      nullptr, nullptr, nullptr, nullptr);
  BoxWorld src;
  src.box.words[0] = addr(alloc_cell(src.w, &good));
  GRHEAP_Image * image = nullptr;
  ASSERT_EQ(grheap_image_take(src.w.heap, nullptr, &image), GRHEAP_OK);
  GRHEAP_Type bad = good;
  bad.struct_size = 0; // a type that forgot its size
  g_resolved = &bad;
  GRHEAP_RestoreEnv env = GRHEAP_RESTORE_ENV_INIT(resolve_it, nullptr);
  {
    BoxWorld dst;
    EXPECT_EQ(grheap_image_instantiate(dst.w.heap, image, &env),
        GRHEAP_ERR_INVALID);
    EXPECT_EQ(dst.w.stats().live_objects, 0u);
  }
  // Control: the same type with its size is accepted.
  g_resolved = &good;
  {
    BoxWorld dst;
    EXPECT_EQ(grheap_image_instantiate(dst.w.heap, image, &env), GRHEAP_OK);
  }
  g_resolved = nullptr;
  grheap_image_release(image);
}

TEST(TypeSize, ATypeFromANewerHeaderIsAcceptedAndItsUnknownTailIgnored) {
  // Decision: accepted, as for a key. A member this library does not know is
  // one whose absence is a defined degradation (a hook not run, no weak
  // slot), so refusing the type would only make a newer embedder unusable on
  // an older heap.
  reset_counts();
  const size_t extra = 2 * sizeof(void *);
  auto * block =
      static_cast<unsigned char *>(std::malloc(sizeof(GRHEAP_Type) + extra));
  GRHEAP_Type proto = weak_type("newer");
  std::memcpy(block, &proto, sizeof proto);
  // The unknown members hold garbage that would crash if called or followed.
  std::memset(block + sizeof(GRHEAP_Type), 0xA5, extra);
  auto * type = reinterpret_cast<GRHEAP_Type *>(block);
  type->struct_size = sizeof(GRHEAP_Type) + extra;
  EXPECT_TRUE(grheap_type_valid(type));
  {
    BoxWorld w;
    Cell * cell = alloc_cell(w.w, type);
    w.box.words[0] = addr(cell);
    void * target = alloc_leaf(w.w);
    ASSERT_EQ(grheap_store(w.w.heap, cell, &cell->target, target), GRHEAP_OK);
    ASSERT_EQ(grheap_collect(w.w.heap), GRHEAP_OK);
    EXPECT_EQ(cell->target, nullptr) << "the members it knows are used";
    EXPECT_EQ(g_cleared, 1);
  }
  std::free(block);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
