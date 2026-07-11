//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for fmt.hpp polyfill.
//
// Verifies:
//   - fmt::format works with positional arguments.
//   - fmt::format works with named arguments (when YADDNSC_USE_STD_FORMAT is on).
//   - fmt::join works.
//   - fmt::arg creates named arguments.
//   - fmt::format_to writes to output iterator.
// =============================================================================

#include <vector>
#include <string>

#include <gtest/gtest.h>

#include "fmt.hpp"

// ── Positional formatting ────────────────────────────────────────────────────

TEST(FmtPolyfillTest, Format_Integer) {
    const auto result = fmt::format("{}", 42);
    EXPECT_EQ(result, "42");
}

TEST(FmtPolyfillTest, Format_MultipleArgs) {
    const auto result = fmt::format("{} + {} = {}", 1, 2, 3);
    EXPECT_EQ(result, "1 + 2 = 3");
}

TEST(FmtPolyfillTest, Format_String) {
    const auto result = fmt::format("hello {}", "world");
    EXPECT_EQ(result, "hello world");
}

TEST(FmtPolyfillTest, Format_Float) {
    const auto result = fmt::format("{:.1f}", 3.14);
    EXPECT_EQ(result, "3.1");
}

TEST(FmtPolyfillTest, Format_Specifiers) {
    const auto result = fmt::format("{:04d}", 7);
    EXPECT_EQ(result, "0007");
}

// ── Named arguments ──────────────────────────────────────────────────────────

#ifdef YADDNSC_USE_STD_FORMAT

TEST(FmtPolyfillTest, NamedArg_Create) {
    auto arg = fmt::arg("name", "value");
    EXPECT_EQ(arg.name, "name");
    EXPECT_EQ(arg.value, "value");
}

TEST(FmtPolyfillTest, NamedArg_IntegerValue) {
    auto arg = fmt::arg("count", 42);
    EXPECT_EQ(arg.value, "42");
}

#endif  // YADDNSC_USE_STD_FORMAT

// ── fmt::join ────────────────────────────────────────────────────────────────
//
// NOTE: The fmt::join implementation in fmt.hpp uses std::data(item) internally,
// which requires the range's value_type to be string-like. Integer ranges are
// not supported by this implementation (convert via fmt::format first).

TEST(FmtPolyfillTest, Join_EmptyRange) {
    std::vector<std::string> empty;
    const auto result = fmt::format("{}", fmt::join(empty, ","));
    EXPECT_TRUE(result.empty());
}

TEST(FmtPolyfillTest, Join_SingleElement) {
    std::vector<std::string> v{"hello"};
    const auto result = fmt::format("{}", fmt::join(v, ","));
    EXPECT_EQ(result, "hello");
}

TEST(FmtPolyfillTest, Join_MultipleElements) {
    std::vector<std::string> v{"a", "b", "c"};
    const auto result = fmt::format("{}", fmt::join(v, ", "));
    EXPECT_EQ(result, "a, b, c");
}

TEST(FmtPolyfillTest, Join_StringViews) {
    std::vector<std::string_view> v{"x", "y", "z"};
    const auto result = fmt::format("{}", fmt::join(v, "|"));
    EXPECT_EQ(result, "x|y|z");
}

// ── format_to ────────────────────────────────────────────────────────────────

TEST(FmtPolyfillTest, FormatTo_AppendToBackInserter) {
    std::string buf;
    auto it = std::back_inserter(buf);
    fmt::format_to(it, "{}", 42);
    EXPECT_EQ(buf, "42");
}

TEST(FmtPolyfillTest, FormatTo_AppendMultiple) {
    std::string buf = "start";
    auto it = std::back_inserter(buf);
    fmt::format_to(it, "-{}", 99);
    EXPECT_EQ(buf, "start-99");
}
