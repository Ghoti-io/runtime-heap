/**
 * @file
 *
 * Result strings, the version and the allocator type.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/runtime-heap/runtime-heap.h>

#include <gtest/gtest.h>

#include <set>
#include <string>

TEST(Result, EveryCodeHasItsOwnString) {
  std::set<std::string> seen;
  for (int i = 0; i < GRHEAP_RESULT_COUNT; i++) {
    const char * s = grheap_result_string(static_cast<GRHEAP_Result>(i));
    ASSERT_NE(s, nullptr);
    EXPECT_STRNE(s, "Unknown error") << "result " << i;
    EXPECT_TRUE(seen.insert(s).second) << "result " << i << " shares a string";
  }
}

TEST(Result, OutOfRangeIsUnknownNotUndefined) {
  EXPECT_STREQ(grheap_result_string(GRHEAP_RESULT_COUNT), "Unknown error");
  EXPECT_STREQ(grheap_result_string(
                   static_cast<GRHEAP_Result>(GRHEAP_RESULT_COUNT + 1)),
      "Unknown error");
}

TEST(Result, ZeroIsSuccess) {
  EXPECT_EQ(GRHEAP_OK, 0);
  EXPECT_STREQ(grheap_result_string(GRHEAP_OK), "No error");
}

/* CONVENTIONS.md section 5 fixes the vocabulary. This library adds nothing to
 * it, and has none of runtime-core's ERR_GUEST: a collector has no guest. A
 * constant added later fails here rather than being noticed later. */
TEST(Result, TheVocabularyIsTheSuitesAndNothingElse) {
  EXPECT_EQ(GRHEAP_RESULT_COUNT, 9);
}

/* The numbers are the ones every other library has, which is also what lets
 * a runtime-core result be passed through as this library's. */
TEST(Result, ConstantsKeepTheNumbersTheSuiteGivesThem) {
  EXPECT_EQ(GRHEAP_OK, 0);
  EXPECT_EQ(GRHEAP_ERR_IO, 1);
  EXPECT_EQ(GRHEAP_ERR_FORMAT, 2);
  EXPECT_EQ(GRHEAP_ERR_UNSUPPORTED, 3);
  EXPECT_EQ(GRHEAP_ERR_LIMIT, 4);
  EXPECT_EQ(GRHEAP_ERR_CORRUPT, 5);
  EXPECT_EQ(GRHEAP_ERR_OOM, 6);
  EXPECT_EQ(GRHEAP_ERR_INVALID, 7);
  EXPECT_EQ(GRHEAP_ERR_INTERNAL, 8);
}

TEST(Version, StringMatchesTheGeneratedHeader) {
  EXPECT_STREQ(grheap_version_string(), GRHEAP_VERSION_STRING);
  EXPECT_EQ(grheap_version_number(), GRHEAP_VERSION_NUMBER);
  EXPECT_EQ(GRHEAP_VERSION_NUMBER,
      GRHEAP_MAKE_VERSION(GRHEAP_VERSION_MAJOR, GRHEAP_VERSION_MINOR,
          GRHEAP_VERSION_PATCH));
}

TEST(Version, PackingIsOneBytePerComponent) {
  EXPECT_EQ(GRHEAP_MAKE_VERSION(1, 2, 3), 0x010203u);
  EXPECT_LT(GRHEAP_MAKE_VERSION(1, 2, 3), GRHEAP_MAKE_VERSION(1, 3, 0));
}

TEST(Allocator, DefaultIsCutils) {
  const GRHEAP_Allocator * allocator = grheap_allocator_default();
  ASSERT_NE(allocator, nullptr);
  EXPECT_EQ(allocator, gcu_allocator_default());
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
