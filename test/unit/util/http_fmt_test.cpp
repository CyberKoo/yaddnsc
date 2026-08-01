//
// Unit tests for include/http_fmt.hpp — log redaction helpers and the
// HttpRequest formatter.
//
// Verifies:
//   - is_sensitive_param: exact matches, suffix fallbacks, case insensitivity.
//   - is_sensitive_header / redact_header.
//   - is_key_start_char / is_key_char / is_key_position.
//   - redact_body: form shape, JSON shape, all value terminators, edge cases.
//   - redact_url_query.
//   - HttpRequest fmt formatter: method mapping, header redaction, body redaction.
// =============================================================================

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "fmt.hpp"
#include "http_fmt.hpp"
#include "http_type.h"

// ===========================================================================
//  is_sensitive_param
// ===========================================================================

TEST(HttpFmtTest, SensitiveParam_ExactMatches) {
    for (const auto key: {"token", "api_key", "apikey", "auth", "secret", "client_secret",
                          "api_secret", "access_key_secret", "secret_access_key",
                          "password", "passwd", "signature"}) {
        EXPECT_TRUE(Utils::Redact::is_sensitive_param(key)) << key;
    }
}

TEST(HttpFmtTest, SensitiveParam_SuffixMatches) {
    EXPECT_TRUE(Utils::Redact::is_sensitive_param("my_token"));
    EXPECT_TRUE(Utils::Redact::is_sensitive_param("cloudflare_secret"));
    EXPECT_TRUE(Utils::Redact::is_sensitive_param("db_password"));
    EXPECT_TRUE(Utils::Redact::is_sensitive_param("private_key"));
}

TEST(HttpFmtTest, SensitiveParam_CaseInsensitive) {
    EXPECT_TRUE(Utils::Redact::is_sensitive_param("TOKEN"));
    EXPECT_TRUE(Utils::Redact::is_sensitive_param("Api_Key"));
    EXPECT_TRUE(Utils::Redact::is_sensitive_param("My_Token"));
}

TEST(HttpFmtTest, SensitiveParam_NonSensitive) {
    EXPECT_FALSE(Utils::Redact::is_sensitive_param("host"));
    EXPECT_FALSE(Utils::Redact::is_sensitive_param("username"));
    EXPECT_FALSE(Utils::Redact::is_sensitive_param("ttl"));
    EXPECT_FALSE(Utils::Redact::is_sensitive_param(""));
}

// ===========================================================================
//  is_sensitive_header / redact_header
// ===========================================================================

TEST(HttpFmtTest, SensitiveHeader_Matches) {
    EXPECT_TRUE(Utils::Redact::is_sensitive_header("Authorization"));
    EXPECT_TRUE(Utils::Redact::is_sensitive_header("Proxy-Authorization"));
    EXPECT_TRUE(Utils::Redact::is_sensitive_header("Cookie"));
    EXPECT_TRUE(Utils::Redact::is_sensitive_header("x-api-key"));
    EXPECT_TRUE(Utils::Redact::is_sensitive_header("X-Auth-Token"));
    EXPECT_TRUE(Utils::Redact::is_sensitive_header("authorization"));  // case-insensitive
}

TEST(HttpFmtTest, SensitiveHeader_NonSensitive) {
    EXPECT_FALSE(Utils::Redact::is_sensitive_header("Host"));
    EXPECT_FALSE(Utils::Redact::is_sensitive_header("User-Agent"));
    EXPECT_FALSE(Utils::Redact::is_sensitive_header("Content-Type"));
    EXPECT_FALSE(Utils::Redact::is_sensitive_header(""));
}

TEST(HttpFmtTest, RedactHeader_SensitiveKey_Redacts) {
    EXPECT_EQ(Utils::Redact::redact_header("Authorization", "Bearer secret"), "***");
    EXPECT_EQ(Utils::Redact::redact_header("Cookie", "session=abc"), "***");
}

TEST(HttpFmtTest, RedactHeader_NonSensitiveKey_PassThrough) {
    EXPECT_EQ(Utils::Redact::redact_header("Host", "example.com"), "example.com");
    EXPECT_EQ(Utils::Redact::redact_header("Content-Type", "application/json"), "application/json");
}

// ===========================================================================
//  key character predicates
// ===========================================================================

TEST(HttpFmtTest, KeyStartChar) {
    EXPECT_TRUE(Utils::Redact::is_key_start_char('a'));
    EXPECT_TRUE(Utils::Redact::is_key_start_char('Z'));
    EXPECT_TRUE(Utils::Redact::is_key_start_char('_'));
    EXPECT_FALSE(Utils::Redact::is_key_start_char('0'));
    EXPECT_FALSE(Utils::Redact::is_key_start_char('-'));
    EXPECT_FALSE(Utils::Redact::is_key_start_char('.'));
    EXPECT_FALSE(Utils::Redact::is_key_start_char(':'));
}

TEST(HttpFmtTest, KeyChar) {
    EXPECT_TRUE(Utils::Redact::is_key_char('a'));
    EXPECT_TRUE(Utils::Redact::is_key_char('9'));
    EXPECT_TRUE(Utils::Redact::is_key_char('.'));
    EXPECT_TRUE(Utils::Redact::is_key_char('-'));
    EXPECT_TRUE(Utils::Redact::is_key_char('_'));
    EXPECT_FALSE(Utils::Redact::is_key_char(':'));
    EXPECT_FALSE(Utils::Redact::is_key_char('@'));
    EXPECT_FALSE(Utils::Redact::is_key_char('/'));
}

TEST(HttpFmtTest, KeyPosition) {
    EXPECT_TRUE(Utils::Redact::is_key_position("token=a", 0));
    EXPECT_TRUE(Utils::Redact::is_key_position("a&token=b", 2));
    EXPECT_TRUE(Utils::Redact::is_key_position("a,token=b", 2));
    EXPECT_TRUE(Utils::Redact::is_key_position("a token=b", 2));
    EXPECT_TRUE(Utils::Redact::is_key_position("a\ntoken=b", 2));
    EXPECT_TRUE(Utils::Redact::is_key_position("a\rtoken=b", 2));
    EXPECT_FALSE(Utils::Redact::is_key_position("atoken=b", 1));  // 'a' is not a separator
    EXPECT_FALSE(Utils::Redact::is_key_position("=token=b", 1));  // '=' is not a separator
}

// ===========================================================================
//  redact_body — form shape
// ===========================================================================

TEST(HttpFmtTest, RedactBody_FormSingleParam) {
    EXPECT_EQ(Utils::Redact::redact_body("token=abc"), "token=***");
}

TEST(HttpFmtTest, RedactBody_FormMultipleParams) {
    EXPECT_EQ(Utils::Redact::redact_body("token=abc&host=example.com"), "token=***&host=example.com");
}

TEST(HttpFmtTest, RedactBody_FormNonSensitivePassThrough) {
    EXPECT_EQ(Utils::Redact::redact_body("host=example.com&port=80"), "host=example.com&port=80");
}

TEST(HttpFmtTest, RedactBody_FormSuffixKey) {
    EXPECT_EQ(Utils::Redact::redact_body("my_secret=abc"), "my_secret=***");
}

TEST(HttpFmtTest, RedactBody_Form_ValueTerminators) {
    // Every value terminator must stop the redacted span: '&', ',', '}', '\n', '\r'.
    EXPECT_EQ(Utils::Redact::redact_body("token=abc&next=1"), "token=***&next=1");
    EXPECT_EQ(Utils::Redact::redact_body("token=abc,next=1"), "token=***,next=1");
    EXPECT_EQ(Utils::Redact::redact_body("token=abc}next"), "token=***}next");
    EXPECT_EQ(Utils::Redact::redact_body("token=abc\nnext=1"), "token=***\nnext=1");
    EXPECT_EQ(Utils::Redact::redact_body("token=abc\r\nnext=1"), "token=***\r\nnext=1");
}

TEST(HttpFmtTest, RedactBody_Form_TrailingValue) {
    // No terminator at the end of input.
    EXPECT_EQ(Utils::Redact::redact_body("token=abc"), "token=***");
}

// ===========================================================================
//  redact_body — JSON shape
// ===========================================================================

TEST(HttpFmtTest, RedactBody_JsonSensitiveKey) {
    EXPECT_EQ(Utils::Redact::redact_body(R"({"token": "abc"})"), R"({"token": ***})");
}

TEST(HttpFmtTest, RedactBody_JsonSpaceAroundColon) {
    EXPECT_EQ(Utils::Redact::redact_body(R"({"token" : "abc"})"), R"({"token" : ***})");
}

TEST(HttpFmtTest, RedactBody_JsonMultipleSpacesBeforeValue) {
    EXPECT_EQ(Utils::Redact::redact_body(R"({"token":   "abc"})"), R"({"token":   ***})");
}

TEST(HttpFmtTest, RedactBody_JsonCommaSeparated) {
    EXPECT_EQ(Utils::Redact::redact_body(R"({"token":"a", "host":"h"})"),
              R"({"token":***, "host":"h"})");
}

TEST(HttpFmtTest, RedactBody_JsonNonSensitivePassThrough) {
    EXPECT_EQ(Utils::Redact::redact_body(R"({"host": "h", "port": 80})"),
              R"({"host": "h", "port": 80})");
}

TEST(HttpFmtTest, RedactBody_JsonMixed) {
    EXPECT_EQ(Utils::Redact::redact_body(R"({"host":"h","password":"p","ttl":60})"),
              R"({"host":"h","password":***,"ttl":60})");
}

TEST(HttpFmtTest, RedactBody_JsonUnterminatedKey_AppendsRest) {
    EXPECT_EQ(Utils::Redact::redact_body(R"({"unterminated)"), R"({"unterminated)");
}

TEST(HttpFmtTest, RedactBody_QuoteWithoutColon_PassThrough) {
    EXPECT_EQ(Utils::Redact::redact_body(R"("hostname" then text)"), R"("hostname" then text)");
}

// ===========================================================================
//  redact_url_query
// ===========================================================================

TEST(HttpFmtTest, RedactUrlQuery_NoQuery_PassThrough) {
    EXPECT_EQ(Utils::Redact::redact_url_query("https://example.com/update"), "https://example.com/update");
}

TEST(HttpFmtTest, RedactUrlQuery_WithSensitiveQuery) {
    EXPECT_EQ(Utils::Redact::redact_url_query("https://example.com/update?token=abc&host=h"),
              "https://example.com/update?token=***&host=h");
}

TEST(HttpFmtTest, RedactUrlQuery_EmptyQuery) {
    EXPECT_EQ(Utils::Redact::redact_url_query("https://example.com/update?"), "https://example.com/update?");
}

// ===========================================================================
//  HttpRequest formatter
// ===========================================================================

namespace {
    [[nodiscard]] HttpRequest make_request(HttpMethod method, std::optional<std::string> body = std::nullopt) {
        HttpRequest req;
        req.content_type = "application/json";
        req.method = method;
        req.headers.insert({"Host", "example.com"});
        req.headers.insert({"Authorization", "Bearer top-secret"});
        req.body = std::move(body);
        return req;
    }
} // namespace

TEST(HttpFmtTest, Format_AllMethods) {
    // Every HttpMethod maps to its canonical string.
    EXPECT_TRUE(fmt::format("{}", make_request(HttpMethod::GET)).find(R"(method="GET")") != std::string::npos);
    EXPECT_TRUE(fmt::format("{}", make_request(HttpMethod::POST)).find(R"(method="POST")") != std::string::npos);
    EXPECT_TRUE(fmt::format("{}", make_request(HttpMethod::PUT)).find(R"(method="PUT")") != std::string::npos);
    EXPECT_TRUE(fmt::format("{}", make_request(HttpMethod::PATCH)).find(R"(method="PATCH")") != std::string::npos);
    EXPECT_TRUE(fmt::format("{}", make_request(HttpMethod::DEL)).find(R"(method="DELETE")") != std::string::npos);
    EXPECT_TRUE(fmt::format("{}", make_request(HttpMethod::HEAD)).find(R"(method="HEAD")") != std::string::npos);
    EXPECT_TRUE(fmt::format("{}", make_request(HttpMethod::OPTIONS)).find(R"(method="OPTIONS")") != std::string::npos);
}

TEST(HttpFmtTest, Format_RedactsSensitiveHeader) {
    const auto out = fmt::format("{}", make_request(HttpMethod::POST, "{}"));
    EXPECT_TRUE(out.find("Authorization=***") != std::string::npos);
    EXPECT_TRUE(out.find("Bearer top-secret") == std::string::npos);
    // Non-sensitive headers pass through.
    EXPECT_TRUE(out.find("Host=example.com") != std::string::npos);
}

TEST(HttpFmtTest, Format_RedactsSensitiveBody) {
    const auto out = fmt::format("{}", make_request(HttpMethod::POST, R"({"token": "abc"})"));
    EXPECT_TRUE(out.find(R"("token": ***)") != std::string::npos);
    EXPECT_TRUE(out.find("abc") == std::string::npos);
}

TEST(HttpFmtTest, Format_EmptyBody) {
    const auto out = fmt::format("{}", make_request(HttpMethod::GET));
    EXPECT_TRUE(out.find(R"(body="")") != std::string::npos);
}
