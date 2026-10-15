//
// Unit tests for the config diagnostics scanner's token classification
// (src/infrastructure/config/diagnostics/locator.cpp).
//
// locate() is driven directly with the smallest buffer that reaches each
// lexical rule, so a failure names one rule instead of a whole config shape.
// The families covered here are the ones the end-to-end loader tests cannot
// reach incidentally:
//
//   - number literals: the sign, leading-zero, fraction and exponent clauses
//     of value_kind(), each with both its accept and its reject arm;
//   - the token lookahead's boundary test, which is what keeps a bare prefix
//     ("tru", "2x") from being read as a complete literal;
//   - key/value state transitions around ':' and separators, including a
//     member name that is never followed by a colon;
//   - line/column accounting across embedded newlines.
//
// Pure string scanning over literal buffers — no file, no schema, no I/O.
//

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "infrastructure/config/diagnostics/locator.h"
#include "infrastructure/config/diagnostics/types.h"

namespace {

using namespace Config::Diagnostic;

/// locate() at the first value position of `{"k":<token>}`.
[[nodiscard]] Site locate_value_of(std::string_view token) {
    const std::string buffer = std::string{R"({"k":)"} + std::string(token) + '}';
    return locate(buffer, buffer.find(':') + 1);
}

}  // namespace

// ---------------------------------------------------------------------------
// Number literals — value_kind()'s sign / zero / fraction / exponent clauses
// ---------------------------------------------------------------------------

TEST(LocatorToken, Number_AcceptsEveryWellFormedShape) {
    for (const auto* token : {"0", "-0", "42", "-42", "0.5", "-0.5", "3.14159", "1e10", "1E10", "1e+10",
                              "1e-10", "-1.5e+2", "0.0e0", "1234567890"}) {
        SCOPED_TRACE(token);
        EXPECT_EQ(locate_value_of(token).kind, JsonKind::NUMBER) << "rejected a valid number: " << token;
    }
}

TEST(LocatorToken, Number_WithoutAnyDigitAfterTheSign_IsUnexpected) {
    // The sign clause: '-' consumed, no digit behind it.
    for (const auto* token : {"-", "-x", "-}"}) {
        SCOPED_TRACE(token);
        EXPECT_EQ(locate_value_of(token).kind, JsonKind::UNEXPECTED) << "accepted: " << token;
    }
}

TEST(LocatorToken, Number_WithFractionPointButNoFractionDigit_IsUnexpected) {
    // The fraction clause: '.' consumed, no digit behind it.
    for (const auto* token : {"1.", "1.x", "-2."}) {
        SCOPED_TRACE(token);
        EXPECT_EQ(locate_value_of(token).kind, JsonKind::UNEXPECTED) << "accepted: " << token;
    }
}

TEST(LocatorToken, Number_WithExponentMarkerButNoExponentDigit_IsUnexpected) {
    // The exponent clause, in all four spellings of the marker/sign pair.
    for (const auto* token : {"1e", "1E", "1e+", "1e-", "2.5e", "1e+x"}) {
        SCOPED_TRACE(token);
        EXPECT_EQ(locate_value_of(token).kind, JsonKind::UNEXPECTED) << "accepted: " << token;
    }
}

TEST(LocatorToken, LeadingZeroRun_IsUnexpected) {
    // '0' takes the single-digit arm, so a second digit is trailing garbage.
    for (const auto* token : {"01", "-01", "00", "0.1"}) {
        SCOPED_TRACE(token);
        if (std::string_view{token} == "0.1") {
            continue;  // a leading zero followed by a fraction is well formed
        }
        EXPECT_EQ(locate_value_of(token).kind, JsonKind::UNEXPECTED) << "accepted: " << token;
    }
}

// ---------------------------------------------------------------------------
// Literals and the lookahead boundary
// ---------------------------------------------------------------------------

TEST(LocatorToken, Literals_AcceptOnlyTheCompleteLiteral) {
    for (const auto* token : {"true", "false", "null"}) {
        SCOPED_TRACE(token);
        const auto site = locate_value_of(token);
        EXPECT_TRUE(site.kind == JsonKind::BOOLEAN || site.kind == JsonKind::NULL_VALUE);
        EXPECT_FALSE(site.malformed_scalar);
    }
}

TEST(LocatorToken, LiteralPrefix_IsUnexpectedNotATypeGuess) {
    // A prefix that is not a complete literal must not be read as one: the
    // lookahead's boundary test is the only thing standing between "tru" and
    // a confident BOOLEAN verdict.
    for (const auto* token : {"tru", "fals", "nul", "nulll", "truex"}) {
        SCOPED_TRACE(token);
        const auto site = locate_value_of(token);
        EXPECT_EQ(site.kind, JsonKind::UNEXPECTED) << "accepted: " << token;
        EXPECT_TRUE(site.malformed_scalar);
    }
}

TEST(LocatorToken, UnexpectedCharacter_ProducesNeitherKindNorMissingValue) {
    // '@' classifies as UNEXPECTED outright — it is not a value prefix at all,
    // so no container boundary or missing-value fact may be invented.
    const auto site = locate_value_of("@");
    EXPECT_EQ(site.kind, JsonKind::UNEXPECTED);
    EXPECT_EQ(site.missing_value.container, JsonKind::NONE);
    EXPECT_EQ(site.after_value.kind, JsonKind::NONE);
}

// ---------------------------------------------------------------------------
// Empty and whitespace-only values
// ---------------------------------------------------------------------------

TEST(LocatorToken, WhitespaceOnlyValue_IsAMissingValueFactNotAType) {
    // A closing brace where a value is required is unconditional syntax
    // damage: it is a missing-value fact naming the object, never a guess at
    // the element's type and never an indexed element.
    const auto site = locate_value_of("   ");
    EXPECT_EQ(site.missing_value.container, JsonKind::OBJECT);
    EXPECT_EQ(site.missing_value.found, '}');
    ASSERT_EQ(site.missing_value.path.size(), 1U);
    EXPECT_EQ(site.missing_value.path[0].key, "k");
}

TEST(LocatorToken, MemberNameWithoutColon_IsReportedAsSuch) {
    // A complete key that is never followed by ':' is its own fact, distinct
    // from a type mismatch and from a missing value.
    const std::string buffer = R"({"lonely" 1})";
    const auto site = locate(buffer, buffer.find("1"));
    EXPECT_TRUE(site.key_without_colon);
}

TEST(LocatorToken, KeyOffsetAfterQuotePolicy_ReportsTheKeyItself) {
    // With the policy set, an offset landing on the key reports the key rather
    // than scanning forward to the value.
    const std::string buffer = R"({"alpha": [1, 2]})";
    const auto site = locate(buffer, 2, {.key_offset_after_quote = true});
    EXPECT_TRUE(site.at_key);
    EXPECT_EQ(site.key, "alpha");
    EXPECT_TRUE(site.missing_value.container == JsonKind::NONE);
}

// ---------------------------------------------------------------------------
// Line / column accounting
// ---------------------------------------------------------------------------

TEST(LocatorToken, LineColumn_CountsBytesAfterEachNewline) {
    const std::string buffer = "{\n  \"k\": [\n    \"a\",\n    1\n  ]\n}";
    // The element "1" sits on line 4; its byte offset is fixed by the buffer.
    const auto site = locate(buffer, buffer.find("1"));
    EXPECT_EQ(site.line, 4U);
    EXPECT_EQ(site.column, 5U);
}

TEST(LocatorToken, LineColumn_FirstByteOfBuffer_IsOneOne) {
    const std::string buffer = R"({"k":1})";
    const auto site = locate(buffer, 0);
    EXPECT_EQ(site.line, 1U);
    EXPECT_EQ(site.column, 1U);
}

TEST(LocatorToken, OffsetBeyondEnd_ClampsInsteadOfReadingPastTheBuffer) {
    // next_token() clamps to buffer.size(). The observable property here is
    // that the scan stays inside the buffer and still reports a position, not
    // which token it lands on — reading past the end would show up as a
    // position beyond the buffer's own line/column range.
    const std::string buffer = "{\n  \"k\": 1\n}";
    const auto site = locate(buffer, buffer.size() + 100);
    EXPECT_GE(site.line, 1U);
    EXPECT_LE(site.line, 3U);
    EXPECT_GE(site.column, 1U);
}

// ---------------------------------------------------------------------------
// Array element states
// ---------------------------------------------------------------------------

TEST(LocatorToken, NestedArrayElement_RetainsTheFullComponentPath) {
    const std::string buffer = R"({"k":[{"x":[1, 2]}]})";
    const auto site = locate(buffer, buffer.find('1'));
    // k -> element 0 -> x -> element 0
    ASSERT_EQ(site.path.size(), 4U);
    EXPECT_EQ(site.path[0].key, "k");
    EXPECT_TRUE(site.path[1].is_index);
    EXPECT_EQ(site.path[1].index, 0U);
    EXPECT_EQ(site.path[2].key, "x");
    EXPECT_TRUE(site.path[3].is_index);
    EXPECT_EQ(site.path[3].index, 0U);
}

TEST(LocatorToken, ArrayIndex_AdvancesPerElement) {
    const std::string buffer = R"({"k":[0, 1, 2]})";
    const auto site = locate(buffer, buffer.find('2'));
    ASSERT_EQ(site.path.size(), 2U);
    EXPECT_TRUE(site.path[1].is_index);
    EXPECT_EQ(site.path[1].index, 2U);
}

// ---------------------------------------------------------------------------
// Object keys — the scan's early-return paths
// ---------------------------------------------------------------------------

namespace {

/// locate() at the byte offset of `needle` inside `buffer`.
[[nodiscard]] Site locate_at(const std::string& buffer, std::string_view needle) {
    const auto at = buffer.find(needle);
    EXPECT_NE(at, std::string::npos) << "needle absent from buffer: " << needle;
    return locate(buffer, at);
}

}  // namespace

TEST(LocatorKey, OffsetOnAMemberKey_ReportsTheKeyAndItsPath) {
    // The offset lands on the opening quote of a nested member, so the scan
    // returns at the key rather than walking on to the value.
    const std::string buffer = R"({"drivers": {"load": ["a.so"]}})";
    const auto site = locate_at(buffer, "load");

    EXPECT_TRUE(site.at_key);
    EXPECT_EQ(site.key, "load");
    EXPECT_TRUE(site.missing_value.container == JsonKind::NONE);
}

TEST(LocatorKey, OffsetJustInsideAMemberKey_StaysOnThatKey) {
    // An offset a byte into the name is still the same key: the scan reads the
    // key's end and looks past it rather than reporting from the offset.
    const std::string buffer = R"({"drivers": {"load": ["a.so"]}})";
    const auto site = locate(buffer, buffer.find("load") + 2);

    EXPECT_TRUE(site.at_key);
    EXPECT_EQ(site.key, "load");
}

TEST(LocatorKey, MemberKeyWithAnEscapeSequence_IsDecoded) {
    // A key carrying an escape still names one member; the decoded text is
    // what the application layer will look up.
    const std::string buffer = R"({"a\"b": 1})";
    const auto site = locate_at(buffer, "a\\\"b");

    EXPECT_TRUE(site.at_key);
    EXPECT_EQ(site.key, "a\"b");
}

TEST(LocatorKey, MemberKeyWithAnInvalidEscape_IsFlaggedMalformed) {
    // \q is not a JSON escape. The key must not be trusted as decoded text,
    // so the scan reports the string as malformed instead of guessing.
    const std::string buffer = R"({"a\qb": 1})";
    const auto site = locate_at(buffer, "a\\qb");

    EXPECT_TRUE(site.malformed_string);
}

TEST(LocatorKey, TopLevelMemberKeyFollowedByAColon_IsNotFlaggedAsUnterminated) {
    // The key/colon pair is the healthy case: the lookahead must find the
    // colon and leave key_without_colon alone.
    const std::string buffer = R"({"alpha": "beta"})";
    const auto site = locate_at(buffer, "alpha");

    EXPECT_TRUE(site.at_key);
    EXPECT_FALSE(site.key_without_colon);
    EXPECT_EQ(site.key, "alpha");
}

TEST(LocatorKey, KeyWithNoColonAnywhere_IsReportedAsUnterminated) {
    // The same shape, but the document never supplies the ':'.
    const std::string buffer = R"({"alpha" "beta"})";
    const auto site = locate_at(buffer, "alpha");

    EXPECT_TRUE(site.key_without_colon);
}

TEST(LocatorKey, KeyOffsetAfterQuotePolicy_MovesTheReportOntoTheQuote) {
    // With the policy set, an offset that would otherwise walk past the key
    // still reports the key — the point of the policy is that an
    // unknown-key diagnostic lands on the name, not on its value.
    const std::string buffer = R"({"alpha": 1})";
    const auto site = locate(buffer, buffer.find("alpha") + 2, {.key_offset_after_quote = true});

    EXPECT_TRUE(site.at_key);
    EXPECT_EQ(site.key, "alpha");
}

TEST(LocatorKey, NestedMemberKey_RetainsTheEnclosingPath) {
    const std::string buffer = R"({"drivers": {"load": ["a.so"]}})";
    const auto site = locate_at(buffer, "load");

    ASSERT_EQ(site.parent_path.size(), 1U);
    EXPECT_EQ(site.parent_path[0].key, "drivers");
}

TEST(LocatorKey, EmptyObjectClosesCleanlyWithoutInventingAMember) {
    // "{}" is well formed: no key, no value, no missing-value fact.
    const std::string buffer = R"({"drivers": {}})";
    const auto site = locate_at(buffer, "{}");

    EXPECT_EQ(site.missing_value.container, JsonKind::NONE);
}

TEST(LocatorKey, EmptyObjectAtTheFailureOffset_KeepsTheSurroundingPath) {
    // The empty object is the innermost container at the reported offset, so
    // its parent path is still available to the renderer.
    const std::string buffer = R"({"a": {"b": {}}})";
    const auto site = locate(buffer, buffer.size());

    EXPECT_EQ(site.missing_value.container, JsonKind::NONE);
    EXPECT_GE(site.parent_path.size(), 1U);
}

// ---------------------------------------------------------------------------
// Containers and separators — the scan's structural states
// ---------------------------------------------------------------------------

TEST(LocatorStructure, NestedContainersOpenAndCloseWithoutInventingAMember) {
    const std::string buffer = R"({"a": {"b": {"c": 1}}})";
    const auto site = locate_at(buffer, "1");

    EXPECT_EQ(site.missing_value.container, JsonKind::NONE);
    EXPECT_EQ(site.after_value.kind, JsonKind::NONE);
}

TEST(LocatorStructure, ArrayElementAfterAnObjectMember_IsIndexed) {
    const std::string buffer = R"({"load": ["a.so", "b.so"]})";
    const auto site = locate_at(buffer, "b.so");

    ASSERT_EQ(site.path.size(), 2U);
    EXPECT_EQ(site.path[0].key, "load");
    EXPECT_TRUE(site.path[1].is_index);
    EXPECT_EQ(site.path[1].index, 1U);
}

TEST(LocatorStructure, EmptyArrayIsWellFormedAndNotAMissingValue) {
    // '[' immediately followed by ']' with nothing consumed is the one legal
    // close-while-awaiting-a-value.
    const std::string buffer = R"({"load": []})";
    const auto site = locate_at(buffer, "]");

    EXPECT_EQ(site.missing_value.container, JsonKind::NONE);
}

TEST(LocatorStructure, ArrayWithATrailingComma_ReportsTheMissingValue) {
    // A separator where an element is required is unconditional damage.
    const std::string buffer = R"({"load": ["a.so",]})";
    const auto site = locate_at(buffer, "]");

    ASSERT_EQ(site.missing_value.container, JsonKind::ARRAY);
    EXPECT_EQ(site.missing_value.found, ']');
}

TEST(LocatorStructure, ArrayWithALeadingComma_ReportsTheMissingValue) {
    const std::string buffer = R"({"load": [,"a.so"]})";
    const auto site = locate_at(buffer, "a.so");

    EXPECT_EQ(site.missing_value.container, JsonKind::ARRAY);
}

TEST(LocatorStructure, ArrayWithADoubleComma_ReportsTheFirstOne) {
    // Only the first is recorded: the rest of the document is not re-scanned
    // for a second fact.
    const std::string buffer = R"({"load": [1,,2]})";
    const auto site = locate_at(buffer, "2");

    ASSERT_EQ(site.missing_value.container, JsonKind::ARRAY);
    EXPECT_EQ(site.missing_value.found, ',');
}

TEST(LocatorStructure, ObjectMemberMissingItsValue_ReportsTheMemberPathNotTheContainer) {
    // The fact names the member whose value is absent, carrying the enclosing
    // path down to it — not just the innermost container.
    const std::string buffer = R"({"outer":{"key":}})";
    const auto site = locate(buffer, buffer.find('}'));

    ASSERT_EQ(site.missing_value.container, JsonKind::OBJECT);
    EXPECT_EQ(site.missing_value.found, '}');
    ASSERT_EQ(site.missing_value.path.size(), 2U);
    EXPECT_EQ(site.missing_value.path[0].key, "outer");
    EXPECT_EQ(site.missing_value.path[1].key, "key");
}

TEST(LocatorStructure, StraySeparatorRightAfterTheColon_IsAMissingObjectValue) {
    // After ':' the scan is awaiting a value, so a comma there is damage
    // rather than a member boundary.
    const std::string buffer = R"({"outer":{"key":,}})";
    const auto site = locate(buffer, buffer.find(','));

    ASSERT_EQ(site.missing_value.container, JsonKind::OBJECT);
    EXPECT_EQ(site.missing_value.found, ',');
    ASSERT_EQ(site.missing_value.path.size(), 2U);
    EXPECT_EQ(site.missing_value.path[1].key, "key");
}

TEST(LocatorStructure, ABraceWhereAMemberValueIsExpected_NamesTheBraceItself) {
    // The closing brace of the inner object is what was found in the value
    // position, so it is the offending character.
    const std::string buffer = R"({"outer":{"key":}})";
    const auto site = locate(buffer, buffer.find('}'));

    EXPECT_EQ(site.missing_value.found, '}');
    EXPECT_EQ(site.missing_value.line, 1U);
}

TEST(LocatorStructure, ValueFollowedByAnUnexpectedSeparator_RecordsAContainerBoundary) {
    // Two values with nothing between them: a complete value followed by a
    // token that is neither ',' nor the closing bracket.
    const std::string buffer = R"({"load": [1 2]})";
    const auto site = locate_at(buffer, "2");

    EXPECT_EQ(site.after_value.kind, JsonKind::ARRAY);
    EXPECT_EQ(site.missing_value.container, JsonKind::NONE);
}

TEST(LocatorStructure, ObjectWithTwoValuesForOneKey_RecordsAContainerBoundary) {
    const std::string buffer = R"({"a": 1 2})";
    const auto site = locate_at(buffer, "2");

    EXPECT_EQ(site.after_value.kind, JsonKind::OBJECT);
}

TEST(LocatorStructure, MissingValueWinsOverAContainerBoundary) {
    // Both facts are available at this offset; the missing value is the
    // unconditional one, so a recorded boundary must not suppress it.
    const std::string buffer = R"({"outer":{"key":}})";
    const auto site = locate(buffer, buffer.find('}'));

    EXPECT_EQ(site.missing_value.container, JsonKind::OBJECT);
    EXPECT_EQ(site.missing_value.path.size(), 2U);
}

TEST(LocatorStructure, CompleteValueBeforeEndOfBuffer_RecordsNoBoundary) {
    // A separator after the value is legal, so nothing is reported.
    const std::string buffer = R"({"a": 1, "b": 2})";
    const auto site = locate_at(buffer, "b");

    EXPECT_EQ(site.after_value.kind, JsonKind::NONE);
    EXPECT_EQ(site.missing_value.container, JsonKind::NONE);
}
