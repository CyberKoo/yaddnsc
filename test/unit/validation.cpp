//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for util/validation.hpp — Utils::is_valid_domain.
//
// Verifies:
//   - Valid domain names pass.
//   - Invalid domain names (empty, no dots, too long) are rejected.
//   - Edge cases: trailing dot, single-label, TLD-only.
// =============================================================================

#include <string_view>

#include <gtest/gtest.h>

#include "util/validation.hpp"

// ── Valid domains ────────────────────────────────────────────────────────────

TEST(DomainValidationTest, SimpleDomain) {
    EXPECT_TRUE(Utils::is_valid_domain("example.com"));
}

TEST(DomainValidationTest, Subdomain) {
    EXPECT_TRUE(Utils::is_valid_domain("www.example.com"));
}

TEST(DomainValidationTest, DeepSubdomain) {
    EXPECT_TRUE(Utils::is_valid_domain("a.b.c.example.com"));
}

TEST(DomainValidationTest, WithTrailingDot) {
    // RFC 1035 allows a trailing dot for the root.
    EXPECT_TRUE(Utils::is_valid_domain("example.com."));
}

TEST(DomainValidationTest, HyphenatedLabels) {
    EXPECT_TRUE(Utils::is_valid_domain("my-host.example.com"));
}

TEST(DomainValidationTest, SingleCharLabels) {
    EXPECT_TRUE(Utils::is_valid_domain("a.b.cd"));
}

TEST(DomainValidationTest, NumericLabels) {
    EXPECT_TRUE(Utils::is_valid_domain("123.example.com"));
}

TEST(DomainValidationTest, MixedCase_Accepted) {
    EXPECT_TRUE(Utils::is_valid_domain("Example.Com"));
    EXPECT_TRUE(Utils::is_valid_domain("WWW.EXAMPLE.COM"));
    EXPECT_TRUE(Utils::is_valid_domain("My-Host.example.Com"));
}

TEST(DomainValidationTest, MaxLabelLength_63_Accepted) {
    std::string label(63, 'a');
    auto domain = label + ".com";
    EXPECT_TRUE(Utils::is_valid_domain(domain));
}

TEST(DomainValidationTest, LongButValid) {
    // Build a domain that is valid per RFC 1035 length constraints.
    // Use realistic label lengths (max 63 chars per label).
    std::string label(60, 'a');
    std::string domain = label + "." + label + "." + label + ".com";
    ASSERT_TRUE(domain.size() < Utils::DOMAIN_NAME_MAX_LEN);
    EXPECT_TRUE(Utils::is_valid_domain(domain));
}

// ── Invalid domains ──────────────────────────────────────────────────────────

TEST(DomainValidationTest, EmptyString_Rejected) {
    EXPECT_FALSE(Utils::is_valid_domain(""));
}

TEST(DomainValidationTest, NoDot_Rejected) {
    EXPECT_FALSE(Utils::is_valid_domain("example"));
}

TEST(DomainValidationTest, ExceedsMaxLength) {
    std::string long_str(254, 'a');
    long_str.replace(200, 1, ".");
    ASSERT_GT(long_str.size(), Utils::DOMAIN_NAME_MAX_LEN);
    EXPECT_FALSE(Utils::is_valid_domain(long_str));
}

TEST(DomainValidationTest, Label_LeadingHyphen_Rejected) {
    EXPECT_FALSE(Utils::is_valid_domain("-example.com"));
    EXPECT_FALSE(Utils::is_valid_domain("-x.example.com"));
}

TEST(DomainValidationTest, Label_TrailingHyphen_Rejected) {
    EXPECT_FALSE(Utils::is_valid_domain("example-.com"));
    EXPECT_FALSE(Utils::is_valid_domain("x-.example.com"));
    EXPECT_FALSE(Utils::is_valid_domain("example.com-"));
}

TEST(DomainValidationTest, TLD_TooShort_Rejected) {
    EXPECT_FALSE(Utils::is_valid_domain("example.c"));
}

TEST(DomainValidationTest, LabelTooLong) {
    std::string label(64, 'b');  // max label length is 63
    auto domain = label + ".com";
    EXPECT_FALSE(Utils::is_valid_domain(domain));
}

TEST(DomainValidationTest, ConsecutiveDots_Rejected) {
    EXPECT_FALSE(Utils::is_valid_domain("example..com"));
}

TEST(DomainValidationTest, WhitespaceInDomain_Rejected) {
    EXPECT_FALSE(Utils::is_valid_domain("example .com"));
}

TEST(DomainValidationTest, SpecialChars_Rejected) {
    EXPECT_FALSE(Utils::is_valid_domain("exa$mple.com"));
}

TEST(DomainValidationTest, PunycodeTLD_Accepted) {
    // xn-- prefix for internationalised TLDs
    EXPECT_TRUE(Utils::is_valid_domain("example.xn--fiqs8s"));  // .xn--fiqs8s = .中国
    EXPECT_TRUE(Utils::is_valid_domain("example.xn--mgberp4a5d4ar"));  // .xn--mgberp4a5d4ar = .الارقام
}

TEST(DomainValidationTest, SingleLabelDomain_Rejected) {
    // A single label (no dots) is not a valid FQDN per RFC 1035
    EXPECT_FALSE(Utils::is_valid_domain("localhost"));
}

TEST(DomainValidationTest, TrailingDotLongTLD_Accepted) {
    // Long TLD with trailing dot
    EXPECT_TRUE(Utils::is_valid_domain("example.travel."));
}

TEST(DomainValidationTest, NumericTLD_Rejected) {
    // TLD must be at least 2 alpha characters
    EXPECT_FALSE(Utils::is_valid_domain("example.123"));
}

TEST(DomainValidationTest, MaxLength_Exactly253) {
    // Build a domain exactly at the RFC 1035 maximum length (253 chars).
    // Layout: three 63-char labels + separators + 61-char TLD
    //   63 + 1 + 63 + 1 + 63 + 1 + 61 = 253
    std::string label(63, 'a');
    std::string tld(61, 'b');
    std::string domain = label + "." + label + "." + label + "." + tld;
    ASSERT_EQ(domain.size(), static_cast<std::size_t>(Utils::DOMAIN_NAME_MAX_LEN));
    EXPECT_TRUE(Utils::is_valid_domain(domain));
}

TEST(DomainValidationTest, TwoTrailingDots_Rejected) {
    // Two trailing dots is not valid
    EXPECT_FALSE(Utils::is_valid_domain("example.com.."));
}
