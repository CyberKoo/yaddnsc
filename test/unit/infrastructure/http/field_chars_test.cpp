//
// Unit tests for the RFC 9110 field-syntax predicates
// (src/infrastructure/http/protocol/field_chars.hpp).
//
// Covers: is_token accepting exactly the tchar set (alphanumerics plus the
// fifteen punctuation marks of RFC 9110 §5.6.2) and rejecting everything else,
// the empty-name rejection, and is_field_value accepting HTAB and visible
// characters while rejecting the other C0 controls and DEL.
//
// The tchar set is walked one character at a time rather than in bulk: the
// predicate is a short-circuit `||` chain, so a single string only ever reaches
// its first matching comparison. One string per character is what proves each
// link of the chain accepts its own punctuation mark and passes everything
// before it on.
//
// Pure predicates over short literals — no socket, no clock, no filesystem I/O.
//

#include <ios>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "infrastructure/http/protocol/field_chars.hpp"

namespace {

using http::protocol::is_field_value;
using http::protocol::is_token;

/// RFC 9110 §5.6.2 tchar = ALPHA / DIGIT / "!" / "#" / "$" / "%" / "&" /
/// "'" / "*" / "+" / "-" / "." / "^" / "_" / "`" / "|" / "~"
constexpr std::string_view kPunctuation = "!#$%&'*+-.^_`|~";

TEST(HttpFieldChars, AcceptsEveryTcharPunctuationMark) {
    ASSERT_EQ(kPunctuation.size(), 15U);
    for (const auto ch : kPunctuation) {
        EXPECT_TRUE(is_token(std::string_view(&ch, 1))) << "punctuation rejected: " << ch;
    }
}

TEST(HttpFieldChars, AcceptsAlphanumerics) {
    for (const auto ch : std::string_view{"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                          "abcdefghijklmnopqrstuvwxyz"}) {
        EXPECT_TRUE(is_token(std::string_view(&ch, 1))) << "alphanumeric rejected: " << ch;
    }
}

TEST(HttpFieldChars, AcceptsATokenMixingAlphanumericsAndPunctuation) {
    EXPECT_TRUE(is_token("Content-Type"));
    EXPECT_TRUE(is_token("X-Powered-By"));
    EXPECT_TRUE(is_token("a1!#$%&'*+-.^_`|~z9"));
}

TEST(HttpFieldChars, RejectsSeparatorsAndDelimiters) {
    // The separators RFC 9110 §5.6.2 excludes from tchar: SP and the
    // gen-delims. A field name carrying any of them must not parse.
    for (const auto ch : std::string_view{"()<>@,;:\\\"/[]?={} \t"}) {
        EXPECT_FALSE(is_token(std::string_view(&ch, 1))) << "separator accepted: " << ch;
    }
}

TEST(HttpFieldChars, RejectsControlCharacters) {
    // Explicit length: the NUL would otherwise terminate the string_view at
    // construction and turn this into the empty-name case.
    for (const auto ch : std::string_view{"\x00\x01\x1f\x7f", 4}) {
        EXPECT_FALSE(is_token(std::string_view(&ch, 1)));
    }
}

TEST(HttpFieldChars, RejectsHighBytes) {
    // 0x80..0xff must not be accepted through the isalnum(locale) path: the
    // value is passed as unsigned char so a signed char cannot go negative.
    for (int i = 0x80; i <= 0xff; ++i) {
        const auto ch = static_cast<char>(i);
        EXPECT_FALSE(is_token(std::string_view(&ch, 1))) << "high byte accepted: 0x" << std::hex << i;
    }
}

TEST(HttpFieldChars, RejectsTheEmptyName) {
    EXPECT_FALSE(is_token(""));
}

TEST(HttpFieldChars, RejectsANameThatMerelyStartsValid) {
    // The chain must keep scanning past the characters it already accepted.
    // Note "!" is itself a tchar, so it is a *valid* first character — only
    // the trailing "(" makes the name a token violation.
    EXPECT_FALSE(is_token("ok("));
    EXPECT_FALSE(is_token("!ok("));
    EXPECT_FALSE(is_token("Content-Type: text/plain"));
}

TEST(HttpFieldChars, FieldValueAcceptsVisibleCharactersAndHtab) {
    for (const auto ch : std::string_view{"\tabcXYZ019/~!#$%&'*+-._"}) {
        EXPECT_TRUE(is_field_value(std::string_view(&ch, 1))) << "field value rejected: " << ch;
    }
}

TEST(HttpFieldChars, FieldValueAcceptsTheEmptyValue) {
    EXPECT_TRUE(is_field_value(""));
}

TEST(HttpFieldChars, FieldValueRejectsC0ControlsOtherThanHtab) {
    // Every C0 control except HTAB, including NUL and the bare CR/LF pair a
    // header-splitting injection would carry.
    for (int i = 0x00; i < 0x20; ++i) {
        if (i == 0x09) {
            continue;
        }
        const auto ch = static_cast<char>(i);
        EXPECT_FALSE(is_field_value(std::string_view(&ch, 1))) << "control accepted: 0x" << std::hex << i;
    }
    EXPECT_FALSE(is_field_value("value\r\nX-Injected: 1"));
}

TEST(HttpFieldChars, FieldValueRejectsDel) {
    const auto del = static_cast<char>(0x7f);
    EXPECT_FALSE(is_field_value(std::string_view(&del, 1)));
}

TEST(HttpFieldChars, FieldValueAcceptsHighBytes) {
    // Bytes >= 0x80 are not C0 controls, so an opaque octet stream passes.
    for (int i = 0x80; i <= 0xff; ++i) {
        const auto ch = static_cast<char>(i);
        EXPECT_TRUE(is_field_value(std::string_view(&ch, 1))) << "high byte rejected: 0x" << std::hex << i;
    }
}

}  // namespace
