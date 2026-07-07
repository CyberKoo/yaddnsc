//
// Unit tests for include/string_util.hpp — string utility functions.
//
// Verifies:
//   - trim/ltrim/rtrim (view-based and copy)
//   - to_lower/to_upper (in-place and copy)
//   - reverse/reverse_copy
//   - replace/replace_copy/replace_all/replace_first
//   - split / join
//   - contains / count
//   - str_to_bool
// =============================================================================

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "string_util.hpp"

// ===========================================================================
// trim / ltrim / rtrim
// ===========================================================================

TEST(StringUtilTrimTest, Ltrim_RemovesLeadingSpaces) {
    EXPECT_EQ(StringUtil::ltrim("  hello"), "hello");
}

TEST(StringUtilTrimTest, Ltrim_AllSpaces_ReturnsEmpty) {
    EXPECT_TRUE(StringUtil::ltrim("   ").empty());
}

TEST(StringUtilTrimTest, Ltrim_NoLeadingSpaces_Unchanged) {
    EXPECT_EQ(StringUtil::ltrim("hello"), "hello");
}

TEST(StringUtilTrimTest, Rtrim_RemovesTrailingSpaces) {
    EXPECT_EQ(StringUtil::rtrim("hello  "), "hello");
}

TEST(StringUtilTrimTest, Rtrim_AllSpaces_ReturnsEmpty) {
    EXPECT_TRUE(StringUtil::rtrim("   ").empty());
}

TEST(StringUtilTrimTest, Rtrim_NoTrailingSpaces_Unchanged) {
    EXPECT_EQ(StringUtil::rtrim("hello"), "hello");
}

TEST(StringUtilTrimTest, Trim_RemovesBothSides) {
    EXPECT_EQ(StringUtil::trim("  hello  "), "hello");
}

TEST(StringUtilTrimTest, Trim_MixedWhitespace) {
    EXPECT_EQ(StringUtil::trim("\t\n hello \r\n"), "hello");
}

TEST(StringUtilTrimTest, Trim_EmptyString_ReturnsEmpty) {
    EXPECT_TRUE(StringUtil::trim("").empty());
}

TEST(StringUtilTrimTest, Trim_NoWhitespace_Unchanged) {
    EXPECT_EQ(StringUtil::trim("hello world"), "hello world");
}

TEST(StringUtilTrimTest, LtrimCopy_ReturnsNewString) {
    auto result = StringUtil::ltrim_copy("  hello");
    EXPECT_EQ(result, "hello");
    // Verify it's an independent std::string, not a view
    EXPECT_TRUE((std::is_same_v<decltype(result), std::string>));
}

TEST(StringUtilTrimTest, RtrimCopy_ReturnsNewString) {
    EXPECT_EQ(StringUtil::rtrim_copy("hello  "), "hello");
}

TEST(StringUtilTrimTest, TrimCopy_ReturnsNewString) {
    EXPECT_EQ(StringUtil::trim_copy("  hello  "), "hello");
}

// ===========================================================================
// to_lower / to_upper
// ===========================================================================

TEST(StringUtilCaseTest, ToLower_InPlace) {
    std::string s = "HELLO World!";
    StringUtil::to_lower(s);
    EXPECT_EQ(s, "hello world!");
}

TEST(StringUtilCaseTest, ToUpper_InPlace) {
    std::string s = "Hello World!";
    StringUtil::to_upper(s);
    EXPECT_EQ(s, "HELLO WORLD!");
}

TEST(StringUtilCaseTest, ToLower_InPlace_AlreadyLower_Unchanged) {
    std::string s = "hello";
    StringUtil::to_lower(s);
    EXPECT_EQ(s, "hello");
}

TEST(StringUtilCaseTest, ToUpper_InPlace_AlreadyUpper_Unchanged) {
    std::string s = "HELLO";
    StringUtil::to_upper(s);
    EXPECT_EQ(s, "HELLO");
}

TEST(StringUtilCaseTest, ToLower_EmptyString) {
    std::string s;
    StringUtil::to_lower(s);
    EXPECT_TRUE(s.empty());
}

TEST(StringUtilCaseTest, ToLowerCopy_ReturnsNewString) {
    auto result = StringUtil::to_lower_copy("HELLO");
    EXPECT_EQ(result, "hello");
    EXPECT_TRUE((std::is_same_v<decltype(result), std::string>));
}

TEST(StringUtilCaseTest, ToUpperCopy_ReturnsNewString) {
    EXPECT_EQ(StringUtil::to_upper_copy("hello"), "HELLO");
}

TEST(StringUtilCaseTest, ToLowerCopy_NonAsciiPreserved) {
    // Non-ASCII bytes should pass through unchanged (lower_table maps them 1:1)
    auto result = StringUtil::to_lower_copy("\xC3\x9C");  // Ü in UTF-8
    EXPECT_EQ(result, "\xC3\x9C");
}

// ===========================================================================
// reverse / reverse_copy
// ===========================================================================

TEST(StringUtilReverseTest, Reverse_InPlace) {
    std::string s = "hello";
    StringUtil::reverse(s);
    EXPECT_EQ(s, "olleh");
}

TEST(StringUtilReverseTest, Reverse_EmptyString) {
    std::string s;
    StringUtil::reverse(s);
    EXPECT_TRUE(s.empty());
}

TEST(StringUtilReverseTest, Reverse_SingleChar) {
    std::string s = "a";
    StringUtil::reverse(s);
    EXPECT_EQ(s, "a");
}

TEST(StringUtilReverseTest, Reverse_Palindrome) {
    std::string s = "racecar";
    StringUtil::reverse(s);
    EXPECT_EQ(s, "racecar");
}

TEST(StringUtilReverseTest, ReverseCopy_ReturnsNewString) {
    EXPECT_EQ(StringUtil::reverse_copy("hello"), "olleh");
}

// ===========================================================================
// replace (pair list)
// ===========================================================================

TEST(StringUtilReplaceTest, Replace_SinglePair) {
    std::string s = "hello world";
    StringUtil::replace(s, std::vector<std::pair<std::string, std::string>>{{"world", "there"}});
    EXPECT_EQ(s, "hello there");
}

TEST(StringUtilReplaceTest, Replace_MultiplePairs) {
    std::string s = "a1b2c3";
    StringUtil::replace(s, std::vector<std::pair<std::string, std::string>>{
        {"1", "one"}, {"2", "two"}, {"3", "three"}
    });
    // replace replaces ALL occurrences of each target sequentially:
    //   "a1b2c3" -> "aoneb2c3" (1->one) -> "aonebtwoc3" (2->two) -> "aonebtwocthree" (3->three)
    EXPECT_EQ(s, "aonebtwocthree");
}

TEST(StringUtilReplaceTest, Replace_EqualLength_OverwritesInPlace) {
    std::string s = "aaabbb";
    StringUtil::replace(s, std::vector<std::pair<std::string, std::string>>{{"aaa", "xxx"}});
    EXPECT_EQ(s, "xxxbbb");
}

TEST(StringUtilReplaceTest, Replace_EmptyTarget_Skipped) {
    std::string s = "hello";
    StringUtil::replace(s, std::vector<std::pair<std::string, std::string>>{{"", "x"}});
    EXPECT_EQ(s, "hello");
}

TEST(StringUtilReplaceTest, Replace_TargetNotFound_Unchanged) {
    std::string s = "hello";
    StringUtil::replace(s, std::vector<std::pair<std::string, std::string>>{{"xyz", "abc"}});
    EXPECT_EQ(s, "hello");
}

TEST(StringUtilReplaceTest, ReplaceCopy_ReturnsNewString) {
    auto result = StringUtil::replace_copy("hello world",
        std::vector<std::pair<std::string, std::string>>{{"world", "there"}});
    EXPECT_EQ(result, "hello there");
    // Original unchanged
    std::string_view original = "hello world";
    (void)original;
}

// ===========================================================================
// replace_all (single target/replacement)
// ===========================================================================

TEST(StringUtilReplaceAllTest, ReplaceAll_MultipleOccurrences) {
    std::string s = "a b a b a";
    StringUtil::replace_all(s, "a", "x");
    EXPECT_EQ(s, "x b x b x");
}

TEST(StringUtilReplaceAllTest, ReplaceAll_NoMatch_Unchanged) {
    std::string s = "hello";
    StringUtil::replace_all(s, "z", "x");
    EXPECT_EQ(s, "hello");
}

TEST(StringUtilReplaceAllTest, ReplaceAll_EmptyTarget_NoOp) {
    std::string s = "hello";
    StringUtil::replace_all(s, "", "x");
    EXPECT_EQ(s, "hello");
}

TEST(StringUtilReplaceAllTest, ReplaceAll_EqualLength_InPlace) {
    std::string s = "010203";
    StringUtil::replace_all(s, "0", "9");
    EXPECT_EQ(s, "919293");
}

TEST(StringUtilReplaceAllTest, ReplaceAll_ShorterReplacement) {
    std::string s = "a--b--c";
    StringUtil::replace_all(s, "--", "-");
    EXPECT_EQ(s, "a-b-c");
}

TEST(StringUtilReplaceAllTest, ReplaceAll_LongerReplacement) {
    std::string s = "a-b-c";
    StringUtil::replace_all(s, "-", " -- ");
    EXPECT_EQ(s, "a -- b -- c");
}

TEST(StringUtilReplaceAllTest, ReplaceAll_OverlappingNotDoubleCounted) {
    // "aaa" with target "aa" should replace to "xa", not "xx" or overwrite the second "aa"
    std::string s = "aaa";
    StringUtil::replace_all(s, "aa", "x");
    EXPECT_EQ(s, "xa");
}

TEST(StringUtilReplaceAllTest, ReplaceAllCopy_ReturnsNewString) {
    auto result = StringUtil::replace_all_copy("a b a", "a", "x");
    EXPECT_EQ(result, "x b x");
}

// ===========================================================================
// replace_first
// ===========================================================================

TEST(StringUtilReplaceFirstTest, ReplaceFirst_ReplacesFirstOnly) {
    std::string s = "a b a b a";
    auto replaced = StringUtil::replace_first(s, "a", "x");
    EXPECT_TRUE(replaced);
    EXPECT_EQ(s, "x b a b a");
}

TEST(StringUtilReplaceFirstTest, ReplaceFirst_NoMatch_ReturnsFalse) {
    std::string s = "hello";
    auto replaced = StringUtil::replace_first(s, "z", "x");
    EXPECT_FALSE(replaced);
    EXPECT_EQ(s, "hello");
}

TEST(StringUtilReplaceFirstTest, ReplaceFirst_EmptyTarget_ReturnsFalse) {
    std::string s = "hello";
    auto replaced = StringUtil::replace_first(s, "", "x");
    EXPECT_FALSE(replaced);
    EXPECT_EQ(s, "hello");
}

// ===========================================================================
// split
// ===========================================================================

TEST(StringUtilSplitTest, Split_BySpace) {
    auto parts = StringUtil::split("hello world foo");
    std::vector<std::string> expected = {"hello", "world", "foo"};
    EXPECT_EQ(parts, expected);
}

TEST(StringUtilSplitTest, Split_ByComma) {
    auto parts = StringUtil::split("a,b,c", ",");
    std::vector<std::string> expected = {"a", "b", "c"};
    EXPECT_EQ(parts, expected);
}

TEST(StringUtilSplitTest, Split_EmptyString_ReturnsEmpty) {
    auto parts = StringUtil::split("");
    EXPECT_TRUE(parts.empty());
}

TEST(StringUtilSplitTest, Split_ConsecutiveDelimiters_SkipsEmpty) {
    auto parts = StringUtil::split("a,,b,,,c", ",");
    std::vector<std::string> expected = {"a", "b", "c"};
    EXPECT_EQ(parts, expected);
}

TEST(StringUtilSplitTest, Split_NoDelimiter_ReturnsWhole) {
    auto parts = StringUtil::split("hello", ",");
    std::vector<std::string> expected = {"hello"};
    EXPECT_EQ(parts, expected);
}

TEST(StringUtilSplitTest, Split_LeadingDelimiter) {
    auto parts = StringUtil::split(",a,b", ",");
    std::vector<std::string> expected = {"a", "b"};
    EXPECT_EQ(parts, expected);
}

TEST(StringUtilSplitTest, Split_TrailingDelimiter) {
    auto parts = StringUtil::split("a,b,", ",");
    std::vector<std::string> expected = {"a", "b"};
    EXPECT_EQ(parts, expected);
}

TEST(StringUtilSplitTest, Split_EmptyDelimiter_Throws) {
    EXPECT_THROW(StringUtil::split("hello", ""), std::invalid_argument);
}

TEST(StringUtilSplitTest, Split_MultiCharDelimiter) {
    auto parts = StringUtil::split("a||b||c", "||");
    std::vector<std::string> expected = {"a", "b", "c"};
    EXPECT_EQ(parts, expected);
}

// ===========================================================================
// join
// ===========================================================================

TEST(StringUtilJoinTest, Join_TwoElements) {
    std::vector<std::string> parts = {"hello", "world"};
    auto result = StringUtil::join(parts, " ");
    EXPECT_EQ(result, "hello world");
}

TEST(StringUtilJoinTest, Join_EmptyVector_ReturnsEmpty) {
    std::vector<std::string> parts;
    auto result = StringUtil::join(parts, ",");
    EXPECT_TRUE(result.empty());
}

TEST(StringUtilJoinTest, Join_SingleElement_NoDelimiter) {
    std::vector<std::string> parts = {"only"};
    auto result = StringUtil::join(parts, ",");
    EXPECT_EQ(result, "only");
}

TEST(StringUtilJoinTest, Join_MultipleElements) {
    std::vector<std::string> parts = {"a", "b", "c", "d"};
    auto result = StringUtil::join(parts, ", ");
    EXPECT_EQ(result, "a, b, c, d");
}

TEST(StringUtilJoinTest, Join_WithEmptyStrings) {
    std::vector<std::string> parts = {"a", "", "c"};
    auto result = StringUtil::join(parts, ":");
    EXPECT_EQ(result, "a::c");
}

TEST(StringUtilJoinTest, Join_IntegersViaStringView) {
    // join works with any StringViewable value type
    std::vector<std::string_view> parts = {"x", "y", "z"};
    auto result = StringUtil::join(parts, "-");
    EXPECT_EQ(result, "x-y-z");
}

// ===========================================================================
// contains
// ===========================================================================

TEST(StringUtilContainsTest, Contains_SubstringPresent) {
    EXPECT_TRUE(StringUtil::contains("hello world", "world"));
}

TEST(StringUtilContainsTest, Contains_SubstringAbsent) {
    EXPECT_FALSE(StringUtil::contains("hello world", "xyz"));
}

TEST(StringUtilContainsTest, Contains_EmptyHaystack) {
    EXPECT_FALSE(StringUtil::contains("", "hello"));
}

TEST(StringUtilContainsTest, Contains_EmptyNeedle_AlwaysTrue) {
    // std::string_view::find("") returns 0
    EXPECT_TRUE(StringUtil::contains("hello", ""));
}

TEST(StringUtilContainsTest, Contains_ExactMatch) {
    EXPECT_TRUE(StringUtil::contains("hello", "hello"));
}

// ===========================================================================
// count
// ===========================================================================

TEST(StringUtilCountTest, Count_NonOverlapping) {
    auto n = StringUtil::count("ababab", "ab");
    EXPECT_EQ(n, 3U);
}

TEST(StringUtilCountTest, Count_NoMatch_ReturnsZero) {
    auto n = StringUtil::count("hello", "z");
    EXPECT_EQ(n, 0U);
}

TEST(StringUtilCountTest, Count_EmptyHaystack_ReturnsZero) {
    auto n = StringUtil::count("", "a");
    EXPECT_EQ(n, 0U);
}

TEST(StringUtilCountTest, Count_EmptyNeedle_ReturnsZero) {
    auto n = StringUtil::count("hello", "");
    EXPECT_EQ(n, 0U);
}

TEST(StringUtilCountTest, Count_SingleChar) {
    auto n = StringUtil::count("aabbccaa", "a");
    EXPECT_EQ(n, 4U);
}

TEST(StringUtilCountTest, Count_NonOverlapping_DoesNotDoubleCount) {
    // "aaa" with "aa" should count as 1, not 2 (non-overlapping)
    auto n = StringUtil::count("aaa", "aa");
    EXPECT_EQ(n, 1U);
}

// ===========================================================================
// str_to_bool
// ===========================================================================

TEST(StringUtilStrToBoolTest, TrueValues) {
    EXPECT_TRUE(StringUtil::str_to_bool("1"));
    EXPECT_TRUE(StringUtil::str_to_bool("on"));
    EXPECT_TRUE(StringUtil::str_to_bool("ON"));
    EXPECT_TRUE(StringUtil::str_to_bool("yes"));
    EXPECT_TRUE(StringUtil::str_to_bool("YES"));
    EXPECT_TRUE(StringUtil::str_to_bool("true"));
    EXPECT_TRUE(StringUtil::str_to_bool("TRUE"));
}

TEST(StringUtilStrToBoolTest, FalseValues) {
    EXPECT_FALSE(StringUtil::str_to_bool("0"));
    EXPECT_FALSE(StringUtil::str_to_bool("off"));
    EXPECT_FALSE(StringUtil::str_to_bool("false"));
    EXPECT_FALSE(StringUtil::str_to_bool("False"));
    EXPECT_FALSE(StringUtil::str_to_bool("no"));
    EXPECT_FALSE(StringUtil::str_to_bool("NO"));
    EXPECT_FALSE(StringUtil::str_to_bool(""));
    EXPECT_FALSE(StringUtil::str_to_bool("random"));
}

// ===========================================================================
// Compile-time checks (constexpr)
// ===========================================================================

TEST(StringUtilConstexprTest, Contains_Constexpr) {
    constexpr bool result = StringUtil::contains("hello world", "world");
    EXPECT_TRUE(result);
}

TEST(StringUtilConstexprTest, Count_Constexpr) {
    constexpr size_t result = StringUtil::count("ababab", "ab");
    EXPECT_EQ(result, 3U);
}

TEST(StringUtilConstexprTest, StrToBool_Constexpr) {
    constexpr bool t = StringUtil::str_to_bool("true");
    constexpr bool f = StringUtil::str_to_bool("false");
    EXPECT_TRUE(t);
    EXPECT_FALSE(f);
}
