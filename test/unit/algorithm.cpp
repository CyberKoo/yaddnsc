//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for util/algorithm.hpp — Utils::dedupe.
//
// Verifies:
//   - Empty vector is unchanged.
//   - Vector with all unique elements is unchanged.
//   - Duplicates are removed (first occurrence preserved).
//   - Multiple duplicates of the same value.
//   - String deduplication.
//   - Return value equals number of removed elements.
// =============================================================================

#include <vector>
#include <string>

#include <gtest/gtest.h>

#include "util/algorithm.hpp"

TEST(DedupeTest, EmptyVector) {
    std::vector<int> v;
    const auto removed = Utils::dedupe(v);
    EXPECT_EQ(removed, 0U);
    EXPECT_TRUE(v.empty());
}

TEST(DedupeTest, SingleElement) {
    std::vector<int> v{42};
    const auto removed = Utils::dedupe(v);
    EXPECT_EQ(removed, 0U);
    ASSERT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0], 42);
}

TEST(DedupeTest, AllUnique) {
    std::vector<int> v{1, 2, 3, 4, 5};
    const auto removed = Utils::dedupe(v);
    EXPECT_EQ(removed, 0U);
    ASSERT_EQ(v.size(), 5U);
}

TEST(DedupeTest, AdjacentDuplicates) {
    std::vector<int> v{1, 2, 2, 3, 4};
    const auto removed = Utils::dedupe(v);
    EXPECT_EQ(removed, 1U);
    ASSERT_EQ(v.size(), 4U);
    EXPECT_EQ(v[0], 1);
    EXPECT_EQ(v[1], 2);
    EXPECT_EQ(v[2], 3);
    EXPECT_EQ(v[3], 4);
}

TEST(DedupeTest, NonAdjacentDuplicates) {
    std::vector<int> v{1, 2, 3, 2, 4, 2};
    const auto removed = Utils::dedupe(v);
    EXPECT_EQ(removed, 2U);
    ASSERT_EQ(v.size(), 4U);
    EXPECT_EQ(v[0], 1);
    EXPECT_EQ(v[1], 2);
    EXPECT_EQ(v[2], 3);
    EXPECT_EQ(v[3], 4);
}

TEST(DedupeTest, AllDuplicates) {
    std::vector<int> v{7, 7, 7, 7};
    const auto removed = Utils::dedupe(v);
    EXPECT_EQ(removed, 3U);
    ASSERT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0], 7);
}

TEST(DedupeTest, StringDuplicates) {
    std::vector<std::string> v{"a", "b", "a", "c", "b", "a"};
    const auto removed = Utils::dedupe(v);
    EXPECT_EQ(removed, 3U);
    ASSERT_EQ(v.size(), 3U);
    EXPECT_EQ(v[0], "a");
    EXPECT_EQ(v[1], "b");
    EXPECT_EQ(v[2], "c");
}

TEST(DedupeTest, OrderPreserved) {
    std::vector<int> v{3, 1, 2, 3, 1, 4};
    const auto removed = Utils::dedupe(v);
    EXPECT_EQ(removed, 2U);
    ASSERT_EQ(v.size(), 4U);
    EXPECT_EQ(v[0], 3);  // first occurrence preserved
    EXPECT_EQ(v[1], 1);
    EXPECT_EQ(v[2], 2);
    EXPECT_EQ(v[3], 4);
}
