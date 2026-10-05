/**
 * @file
 *
 * An image restore environment states its own size (image.h), so the struct
 * can grow at the end. No member has been added after `user` yet, so there is
 * no older layout to copy; what is tested is the rule that holds the door
 * open: the initialiser writes the size, a size below the struct or off its
 * alignment (zero is a struct filled by assignment that forgot its size) is
 * refused and not read, and an environment from a newer header is accepted
 * with its unknown tail ignored. Each refusal has a full-size control.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

#include <ghoti.io/runtime-core/b/roots.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace {

int g_resolutions = 0;

const GRHEAP_Type * resolve_leaf(void *, const char * name) {
  g_resolutions++;
  return std::strcmp(name, kLeaf.name) == 0 ? &kLeaf : nullptr;
}

struct RootBox {
  std::vector<uint64_t> words;
};
void box_enumerate(
    GRCORE_Context *, void * value, const GRCORE_RootVisitor * visitor) {
  for (uint64_t & w : static_cast<RootBox *>(value)->words) {
    visitor->slot(visitor->user, &w);
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

// An image of one leaf.
struct Imaged {
  BoxWorld src;
  GRHEAP_Image * image = nullptr;
  Imaged() {
    void * p = nullptr;
    EXPECT_EQ(grheap_alloc(src.w.heap, &kLeaf, &p), GRHEAP_OK);
    src.box.words[0] = addr(p);
    EXPECT_EQ(grheap_image_take(src.w.heap, nullptr, &image), GRHEAP_OK);
  }
  ~Imaged() { grheap_image_release(image); }
};

} // namespace

TEST(HeapRestoreEnvSize, TheInitialiserWritesTheSizeOfTheStructItWasCompiledAgainst) {
  static const GRHEAP_RestoreEnv e =
      GRHEAP_RESTORE_ENV_INIT(resolve_leaf, nullptr);
  EXPECT_EQ(e.size, sizeof(GRHEAP_RestoreEnv));
  EXPECT_GE(e.size, GRHEAP_RESTORE_ENV_MIN_SIZE);
  EXPECT_TRUE(grheap_restore_env_valid(&e));
  EXPECT_FALSE(grheap_restore_env_valid(nullptr));
}

TEST(HeapRestoreEnvSize, ASizeBelowTheMinimumOrOffAlignmentIsRefusedNotRead) {
  Imaged im;
  const size_t bad[] = {0, 1, GRHEAP_RESTORE_ENV_MIN_SIZE - sizeof(void *),
      GRHEAP_RESTORE_ENV_MIN_SIZE - 1, GRHEAP_RESTORE_ENV_MIN_SIZE + 1,
      sizeof(GRHEAP_RestoreEnv) + 1};
  for (size_t size : bad) {
    GRHEAP_RestoreEnv e = GRHEAP_RESTORE_ENV_INIT(resolve_leaf, nullptr);
    e.size = size;
    EXPECT_FALSE(grheap_restore_env_valid(&e)) << "size " << size;
    BoxWorld dst;
    g_resolutions = 0;
    EXPECT_EQ(grheap_image_instantiate(dst.w.heap, im.image, &e),
        GRHEAP_ERR_INVALID)
        << "size " << size;
    EXPECT_EQ(g_resolutions, 0) << "the resolver is not read or called";
    EXPECT_EQ(dst.w.stats().live_objects, 0u);
  }
  // Controls: the smallest size and the full size are accepted, so the list
  // above is refused for its size and not for anything else about it.
  for (size_t size : {GRHEAP_RESTORE_ENV_MIN_SIZE, sizeof(GRHEAP_RestoreEnv)}) {
    GRHEAP_RestoreEnv e = GRHEAP_RESTORE_ENV_INIT(resolve_leaf, nullptr);
    e.size = size;
    BoxWorld dst;
    g_resolutions = 0;
    EXPECT_EQ(grheap_image_instantiate(dst.w.heap, im.image, &e), GRHEAP_OK)
        << "size " << size;
    EXPECT_EQ(g_resolutions, 1);
  }
  EXPECT_EQ(grheap_image_instantiate(BoxWorld().w.heap, im.image, nullptr),
      GRHEAP_ERR_INVALID);
}

TEST(HeapRestoreEnvSize, AnEnvironmentFromANewerHeaderIsAcceptedAndItsUnknownTailIgnored) {
  // Decision: accepted, as for a key: a member this library does not know is
  // one whose absence is a defined degradation.
  Imaged im;
  const size_t extra = 2 * sizeof(void *);
  auto * block = static_cast<unsigned char *>(
      std::malloc(sizeof(GRHEAP_RestoreEnv) + extra));
  GRHEAP_RestoreEnv proto = GRHEAP_RESTORE_ENV_INIT(resolve_leaf, nullptr);
  std::memcpy(block, &proto, sizeof proto);
  std::memset(block + sizeof(GRHEAP_RestoreEnv), 0xA5, extra);
  auto * e = reinterpret_cast<GRHEAP_RestoreEnv *>(block);
  e->size = sizeof(GRHEAP_RestoreEnv) + extra;
  EXPECT_TRUE(grheap_restore_env_valid(e));
  {
    BoxWorld dst;
    g_resolutions = 0;
    EXPECT_EQ(grheap_image_instantiate(dst.w.heap, im.image, e), GRHEAP_OK);
    EXPECT_EQ(g_resolutions, 1) << "the members it knows are used";
  }
  std::free(block);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
