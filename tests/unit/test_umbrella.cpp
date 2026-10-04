/**
 * @file
 *
 * The umbrella header on its own, compiled as C++, and the rule that it never
 * reaches runtime-core's A.
 *
 * Copyright 2026 by Corey Pennycuff
 */

// Only the umbrella: nothing else may be needed to use the whole API.
#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#define GRHEAP_TEST_STR2(x) #x
#define GRHEAP_TEST_EXPAND(x) GRHEAP_TEST_STR2(x)

TEST(Umbrella, ReachesTheResultVersionAndAllocatorDeclarations) {
  EXPECT_STREQ(grheap_result_string(GRHEAP_OK), "No error");
  EXPECT_NE(grheap_version_string(), nullptr);
  EXPECT_NE(grheap_allocator_default(), nullptr);
}

TEST(Umbrella, ReachesEveryHeaderOfTheAPI) {
  // One symbol from each of options.h, heap.h, roots.h, store.h and type.h,
  // through the umbrella alone.
  GRHEAP_ValueCodec codec;
  grheap_value_codec_default(&codec);
  EXPECT_EQ(codec.mask, UINT64_MAX);
  EXPECT_EQ(grheap_heap_get(nullptr), nullptr);
  EXPECT_EQ(grheap_handle_get(nullptr), nullptr);
  EXPECT_EQ(grheap_store(nullptr, nullptr, nullptr, nullptr),
      GRHEAP_ERR_INVALID);
  grheap_trace_slot(nullptr, nullptr); // ignored
  GRHEAP_Type type = {"t", 8, nullptr, nullptr, nullptr, nullptr, nullptr};
  EXPECT_EQ(type.size, 8u);
  // id.h, the weak spec in type.h, the instruments in options.h and store.h.
  uint64_t id = 0;
  EXPECT_EQ(grheap_id_of(nullptr, nullptr, &id), GRHEAP_ERR_INVALID);
  uint64_t violations = 0;
  EXPECT_EQ(grheap_verify(nullptr, &violations), GRHEAP_ERR_INVALID);
  GRHEAP_WeakSpec weak = {GRHEAP_WEAK_CELL, 0, 0, 0, false, nullptr};
  EXPECT_EQ(weak.kind, GRHEAP_WEAK_CELL);
  EXPECT_EQ(grheap_options_set_torture(nullptr, true), GRHEAP_ERR_INVALID);
}

TEST(Umbrella, ReachesTheRetentionQuery) {
  // retention.h through the umbrella alone, from C++.
  EXPECT_FALSE(grheap_retention_retained(nullptr));
  EXPECT_EQ(grheap_retention_step_count(nullptr), 0u);
  GRHEAP_Retention * path = nullptr;
  EXPECT_EQ(grheap_retention_path(nullptr, nullptr, nullptr, &path),
      GRHEAP_ERR_INVALID);
  EXPECT_STREQ(grheap_retention_root_kind_string(GRHEAP_ROOT_HANDLE), "handle");
}

TEST(Umbrella, ReachesHeapImages) {
  // image.h through the umbrella alone.
  EXPECT_EQ(grheap_image_refcount(nullptr), 0u);
  EXPECT_EQ(grheap_image_retain(nullptr), nullptr);
  EXPECT_EQ(grheap_image_take(nullptr, nullptr, nullptr), GRHEAP_ERR_INVALID);
  GRHEAP_RestoreEnv env = {nullptr, nullptr};
  EXPECT_EQ(grheap_image_instantiate(nullptr, nullptr, &env), GRHEAP_ERR_INVALID);
  EXPECT_STREQ(GRHEAP_SNAPSHOT_KEY_NAME, "runtime-heap");
}

TEST(Umbrella, TheStabilityConstantsAreThoseTheDocumentationStates) {
  EXPECT_EQ(GRHEAP_HEADER_BYTES, 16u);
  EXPECT_EQ(GRHEAP_SMALL_CELL_MAX, 8192u);
  EXPECT_EQ(GRHEAP_PIN_MAX, 255u);
}

TEST(Umbrella, HeadersHaveExternCLinkageSoCppCanLinkTheSymbols) {
  /* A C++ translation unit that declared these without extern "C" would fail
   * to link; reaching this line at all is the check. */
  EXPECT_EQ(grheap_version_number(), GRHEAP_VERSION_NUMBER);
}

/* AD-2: the collector includes no A header, and nothing it includes does. The
 * guard of each A header and of runtime-core's umbrella (which includes all of
 * them) is a macro; if any were defined, this translation unit, which included
 * only the heap's umbrella, would have reached A. */
TEST(Umbrella, DoesNotReachRuntimeCoresAHeadersNorItsUmbrella) {
#ifdef GHOTI_IO_GRCORE_RUNTIME_CORE_H
  FAIL() << "the heap's umbrella includes runtime-core's umbrella";
#endif
#ifdef GHOTI_IO_GRCORE_A_STACK_H
  FAIL() << "the heap's umbrella reaches a/stack.h";
#endif
#ifdef GHOTI_IO_GRCORE_A_FRAME_H
  FAIL() << "the heap's umbrella reaches a/frame.h";
#endif
#ifdef GHOTI_IO_GRCORE_A_UNWIND_H
  FAIL() << "the heap's umbrella reaches a/unwind.h";
#endif
#ifdef GHOTI_IO_GRCORE_A_ENGINE_H
  FAIL() << "the heap's umbrella reaches a/engine.h";
#endif
  // And it did reach B, which is what it is allowed.
#ifndef GHOTI_IO_GRCORE_B_CONTEXT_H
  FAIL() << "the heap's umbrella does not reach b/context.h";
#endif
  SUCCEED();
}

TEST(Umbrella, EveryTypeRenameIsInEffectNotInsideAComment) {
  const char * renamed[] = {GRHEAP_TEST_EXPAND(GRHEAP_Allocator),
      GRHEAP_TEST_EXPAND(GRHEAP_Heap), GRHEAP_TEST_EXPAND(GRHEAP_Options),
      GRHEAP_TEST_EXPAND(GRHEAP_Type), GRHEAP_TEST_EXPAND(GRHEAP_Tracer),
      GRHEAP_TEST_EXPAND(GRHEAP_Handle), GRHEAP_TEST_EXPAND(GRHEAP_Stats),
      GRHEAP_TEST_EXPAND(GRHEAP_ValueCodec), GRHEAP_TEST_EXPAND(GRHEAP_Mode),
      GRHEAP_TEST_EXPAND(GRHEAP_TraceFn), GRHEAP_TEST_EXPAND(GRHEAP_DestroyFn)};
  for (const char * r : renamed) {
    EXPECT_NE(std::string(r).find("ghotiio_"), std::string::npos) << r;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
