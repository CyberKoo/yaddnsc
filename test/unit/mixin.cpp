//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for mixin.h — NoCopy / NoMove tag types.
//
// Verifies:
//   - NoCopy disables copy construction/assignment.
//   - NoCopy preserves move semantics.
//   - NoMove disables move construction/assignment.
//   - NoMove preserves copy semantics.
//   - Mixed inheritance (both tags) disables both.
// =============================================================================

#include <type_traits>

#include <gtest/gtest.h>

#include "mixin.h"

// ---------------------------------------------------------------------------
// NoCopy
// ---------------------------------------------------------------------------

TEST(NoCopyTest, CopyConstructor_Deleted) {
    EXPECT_FALSE(std::is_copy_constructible_v<NoCopy>);
}

TEST(NoCopyTest, CopyAssignment_Deleted) {
    EXPECT_FALSE(std::is_copy_assignable_v<NoCopy>);
}

TEST(NoCopyTest, MoveConstructor_Allowed) {
    EXPECT_TRUE(std::is_move_constructible_v<NoCopy>);
}

TEST(NoCopyTest, MoveAssignment_Allowed) {
    EXPECT_TRUE(std::is_move_assignable_v<NoCopy>);
}

TEST(NoCopyTest, CanDefaultConstruct) {
    NoCopy nc;
    (void)nc;
}

TEST(NoCopyTest, CanMoveConstruct) {
    NoCopy nc1;
    NoCopy nc2(std::move(nc1));
    (void)nc2;
}

TEST(NoCopyTest, CanMoveAssign) {
    NoCopy nc1;
    NoCopy nc2;
    nc2 = std::move(nc1);
}

// ---------------------------------------------------------------------------
// NoMove
// ---------------------------------------------------------------------------

TEST(NoMoveTest, MoveConstructor_Deleted) {
    EXPECT_FALSE(std::is_move_constructible_v<NoMove>);
}

TEST(NoMoveTest, MoveAssignment_Deleted) {
    EXPECT_FALSE(std::is_move_assignable_v<NoMove>);
}

TEST(NoMoveTest, CopyConstructor_Allowed) {
    EXPECT_TRUE(std::is_copy_constructible_v<NoMove>);
}

TEST(NoMoveTest, CopyAssignment_Allowed) {
    EXPECT_TRUE(std::is_copy_assignable_v<NoMove>);
}

TEST(NoMoveTest, CanDefaultConstruct) {
    NoMove nm;
    (void)nm;
}

TEST(NoMoveTest, CanCopyConstruct) {
    NoMove nm1;
    NoMove nm2(nm1);
    (void)nm2;
}

TEST(NoMoveTest, CanCopyAssign) {
    NoMove nm1;
    NoMove nm2;
    nm2 = nm1;
}

// ---------------------------------------------------------------------------
// Combined: class inheriting both NoCopy and NoMove
// ---------------------------------------------------------------------------

struct BothMixins : private NoCopy, private NoMove {
};

TEST(MixinCombinedTest, BothCopyAndMove_Deleted) {
    EXPECT_FALSE(std::is_copy_constructible_v<BothMixins>);
    EXPECT_FALSE(std::is_move_constructible_v<BothMixins>);
    EXPECT_FALSE(std::is_copy_assignable_v<BothMixins>);
    EXPECT_FALSE(std::is_move_assignable_v<BothMixins>);
}

TEST(MixinCombinedTest, Both_CanDefaultConstruct) {
    BothMixins bm;
    (void)bm;
}

// SFINAE-friendly: the tags carry [[no_unique_address]] and add no size overhead.
TEST(MixinCombinedTest, ZeroOverhead) {
    EXPECT_EQ(sizeof(NoCopy), 1U);
    EXPECT_EQ(sizeof(NoMove), 1U);
    EXPECT_LE(sizeof(BothMixins), 2U);  // implementation may pack or not
}
