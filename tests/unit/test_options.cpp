/**
 * @file
 *
 * Heap options: the defaults, the setters and what they refuse.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "test_helpers.h"

namespace {

struct Opts {
  GRHEAP_Options * o = nullptr;
  Opts() { EXPECT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK); }
  ~Opts() { grheap_options_destroy(o); }
};

} // namespace

TEST(Options, DefaultsAreCollectedIdentityCodecThresholdAndAutoCollect) {
  Opts opts;
  EXPECT_EQ(grheap_options_get_mode(opts.o), GRHEAP_MODE_COLLECTED);
  EXPECT_EQ(grheap_options_get_gc_threshold(opts.o), 256u * 1024u);
  EXPECT_EQ(GRHEAP_DEFAULT_GC_THRESHOLD, 256u * 1024u);
  EXPECT_TRUE(grheap_options_get_auto_collect(opts.o));
  GRHEAP_ValueCodec c;
  ASSERT_EQ(grheap_options_get_value_codec(opts.o, &c), GRHEAP_OK);
  // The identity: every word is a pointer, no tag test.
  EXPECT_EQ(c.tag_mask, 0u);
  EXPECT_EQ(c.tag_value, 0u);
  EXPECT_EQ(c.mask, UINT64_MAX);
  EXPECT_EQ(c.shift, 0u);
  EXPECT_EQ(c.base, 0u);
}

TEST(Options, TheCodecDefaultInitialiserMatchesTheOptionsDefault) {
  GRHEAP_ValueCodec c = {1, 1, 1, 1, 1};
  grheap_value_codec_default(&c);
  Opts opts;
  GRHEAP_ValueCodec d;
  ASSERT_EQ(grheap_options_get_value_codec(opts.o, &d), GRHEAP_OK);
  EXPECT_EQ(c.tag_mask, d.tag_mask);
  EXPECT_EQ(c.tag_value, d.tag_value);
  EXPECT_EQ(c.mask, d.mask);
  EXPECT_EQ(c.shift, d.shift);
  EXPECT_EQ(c.base, d.base);
  grheap_value_codec_default(nullptr); // ignored
}

TEST(Options, SettersChangeWhatTheGettersReturn) {
  Opts opts;
  EXPECT_EQ(grheap_options_set_mode(opts.o, GRHEAP_MODE_ARENA), GRHEAP_OK);
  EXPECT_EQ(grheap_options_get_mode(opts.o), GRHEAP_MODE_ARENA);
  EXPECT_EQ(grheap_options_set_gc_threshold(opts.o, 12345), GRHEAP_OK);
  EXPECT_EQ(grheap_options_get_gc_threshold(opts.o), 12345u);
  EXPECT_EQ(grheap_options_set_auto_collect(opts.o, false), GRHEAP_OK);
  EXPECT_FALSE(grheap_options_get_auto_collect(opts.o));
  GRHEAP_ValueCodec in = {1, 1, ~UINT64_C(1), 1, 0x1000};
  EXPECT_EQ(grheap_options_set_value_codec(opts.o, &in), GRHEAP_OK);
  GRHEAP_ValueCodec out;
  ASSERT_EQ(grheap_options_get_value_codec(opts.o, &out), GRHEAP_OK);
  EXPECT_EQ(out.tag_mask, 1u);
  EXPECT_EQ(out.tag_value, 1u);
  EXPECT_EQ(out.mask, ~UINT64_C(1));
  EXPECT_EQ(out.shift, 1u);
  EXPECT_EQ(out.base, 0x1000u);
}

TEST(Options, ARefusedSetterLeavesTheValueAlone) {
  Opts opts;
  ASSERT_EQ(grheap_options_set_mode(opts.o, GRHEAP_MODE_ARENA), GRHEAP_OK);
  EXPECT_EQ(grheap_options_set_mode(opts.o, static_cast<GRHEAP_Mode>(7)),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_get_mode(opts.o), GRHEAP_MODE_ARENA);

  GRHEAP_ValueCodec good = {0, 0, UINT64_MAX, 3, 8};
  ASSERT_EQ(grheap_options_set_value_codec(opts.o, &good), GRHEAP_OK);
  GRHEAP_ValueCodec bad_shift = {0, 0, UINT64_MAX, 64, 0};
  EXPECT_EQ(grheap_options_set_value_codec(opts.o, &bad_shift),
      GRHEAP_ERR_INVALID);
  // A tag value with a bit outside the mask could never be matched.
  GRHEAP_ValueCodec bad_tag = {1, 3, UINT64_MAX, 0, 0};
  EXPECT_EQ(grheap_options_set_value_codec(opts.o, &bad_tag),
      GRHEAP_ERR_INVALID);
  GRHEAP_ValueCodec now;
  ASSERT_EQ(grheap_options_get_value_codec(opts.o, &now), GRHEAP_OK);
  EXPECT_EQ(now.shift, 3u);
  EXPECT_EQ(now.base, 8u);
}

TEST(Options, NullArgumentsAreRefusedOrGiveTheDefault) {
  EXPECT_EQ(grheap_options_create(nullptr, nullptr), GRHEAP_ERR_INVALID);
  GRHEAP_ValueCodec c = {};
  Opts opts;
  EXPECT_EQ(grheap_options_set_mode(nullptr, GRHEAP_MODE_ARENA),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_set_value_codec(nullptr, &c), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_set_value_codec(opts.o, nullptr),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_get_value_codec(opts.o, nullptr),
      GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_get_value_codec(nullptr, &c), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_set_gc_threshold(nullptr, 1), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_set_auto_collect(nullptr, true), GRHEAP_ERR_INVALID);
  EXPECT_EQ(grheap_options_get_mode(nullptr), GRHEAP_MODE_COLLECTED);
  EXPECT_EQ(grheap_options_get_gc_threshold(nullptr),
      GRHEAP_DEFAULT_GC_THRESHOLD);
  EXPECT_TRUE(grheap_options_get_auto_collect(nullptr));
  grheap_options_destroy(nullptr); // ignored
}

TEST(Options, CreateUsesTheAllocatorItIsGivenAndFreesWithIt) {
  TrackingAllocator a;
  GRHEAP_Options * o;
  ASSERT_EQ(grheap_options_create(a.get(), &o), GRHEAP_OK);
  EXPECT_EQ(a.live, 1);
  grheap_options_destroy(o);
  EXPECT_EQ(a.live, 0);
}

TEST(Options, ACreateThatCannotAllocateReportsOomAndWritesNothing) {
  TrackingAllocator a;
  a.fail_at = 1;
  GRHEAP_Options * o = reinterpret_cast<GRHEAP_Options *>(0x1);
  EXPECT_EQ(grheap_options_create(a.get(), &o), GRHEAP_ERR_OOM);
  EXPECT_EQ(o, reinterpret_cast<GRHEAP_Options *>(0x1));
  EXPECT_EQ(a.live, 0);
}

TEST(Options, TheHeapReadsThemOnceAndCopiesWhatItNeeds) {
  WorldConfig config;
  config.create_heap = false;
  HeapWorld w(config);
  GRHEAP_Options * o;
  ASSERT_EQ(grheap_options_create(nullptr, &o), GRHEAP_OK);
  grheap_options_set_mode(o, GRHEAP_MODE_ARENA);
  ASSERT_EQ(grheap_heap_create(w.ctx, o, &w.heap), GRHEAP_OK);
  // Changing or destroying the options afterwards does not touch the heap.
  grheap_options_set_mode(o, GRHEAP_MODE_COLLECTED);
  grheap_options_destroy(o);
  void * p;
  ASSERT_EQ(grheap_alloc(w.heap, &kLeaf, &p), GRHEAP_OK);
  ASSERT_EQ(grheap_collect(w.heap), GRHEAP_OK);
  // An arena never collects: the object is still there.
  EXPECT_TRUE(grheap_contains(w.heap, p));
  EXPECT_EQ(w.stats().collections, 0u);
}

TEST(Options, TheRelocationQuerySaysWhetherThisBuildHasTheTestOnlyMode) {
  // Only a build made with RELOCATE=yes (which defines GRHEAP_RELOCATE for the
  // library and the tests alike) has relocation torture; every other build says
  // so through this one query and carries nothing else of it.
#ifdef GRHEAP_RELOCATE
  EXPECT_TRUE(grheap_relocation_available());
#else
  EXPECT_FALSE(grheap_relocation_available());
#endif
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
