//
// Unit tests for the SDK log-redaction helpers (<yaddnsc/sdk/redact.hpp>),
// request rendering (yaddnsc::sdk::format_request) and form encoding
// (<yaddnsc/sdk/form_encode.hpp>).
//
// Verifies:
//   - is_sensitive_param: exact matches, suffix fallbacks, case insensitivity.
//   - is_sensitive_header / redact_header.
//   - is_key_start_char / is_key_char / is_key_position.
//   - redact_body: form shape, JSON shape, all value terminators, edge cases.
//   - redact_url_query.
//   - format_request: method mapping, header redaction, body redaction.
//   - encode_form_component / encode_form.
// =============================================================================

#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/form_encode.hpp>
#include <yaddnsc/sdk/redact.hpp>

namespace redact = yaddnsc::sdk::redact;

// ===========================================================================
//  is_sensitive_param
// ===========================================================================

TEST(SdkHelpersTest, SensitiveParam_ExactMatches) {
    for (const auto key : {"token", "api_key", "apikey", "auth", "secret", "client_secret", "api_secret",
                           "access_key_secret", "secret_access_key", "password", "passwd", "signature"}) {
        EXPECT_TRUE(redact::is_sensitive_param(key)) << key;
    }
}

TEST(SdkHelpersTest, SensitiveParam_SuffixMatches) {
    EXPECT_TRUE(redact::is_sensitive_param("my_token"));
    EXPECT_TRUE(redact::is_sensitive_param("cloudflare_secret"));
    EXPECT_TRUE(redact::is_sensitive_param("db_password"));
    EXPECT_TRUE(redact::is_sensitive_param("private_key"));
}

TEST(SdkHelpersTest, SensitiveParam_CaseInsensitive) {
    EXPECT_TRUE(redact::is_sensitive_param("TOKEN"));
    EXPECT_TRUE(redact::is_sensitive_param("Api_Key"));
    EXPECT_TRUE(redact::is_sensitive_param("My_Token"));
}

TEST(SdkHelpersTest, SensitiveParam_NonSensitive) {
    EXPECT_FALSE(redact::is_sensitive_param("host"));
    EXPECT_FALSE(redact::is_sensitive_param("username"));
    EXPECT_FALSE(redact::is_sensitive_param("ttl"));
    EXPECT_FALSE(redact::is_sensitive_param(""));
}

// ===========================================================================
//  is_sensitive_header / redact_header
// ===========================================================================

TEST(SdkHelpersTest, SensitiveHeader_Matches) {
    EXPECT_TRUE(redact::is_sensitive_header("Authorization"));
    EXPECT_TRUE(redact::is_sensitive_header("Proxy-Authorization"));
    EXPECT_TRUE(redact::is_sensitive_header("Cookie"));
    EXPECT_TRUE(redact::is_sensitive_header("x-api-key"));
    EXPECT_TRUE(redact::is_sensitive_header("X-Auth-Token"));
    EXPECT_TRUE(redact::is_sensitive_header("authorization"));  // case-insensitive
}

TEST(SdkHelpersTest, SensitiveHeader_NonSensitive) {
    EXPECT_FALSE(redact::is_sensitive_header("Host"));
    EXPECT_FALSE(redact::is_sensitive_header("User-Agent"));
    EXPECT_FALSE(redact::is_sensitive_header("Content-Type"));
    EXPECT_FALSE(redact::is_sensitive_header(""));
}

TEST(SdkHelpersTest, RedactHeader_SensitiveKey_Redacts) {
    EXPECT_EQ(redact::redact_header("Authorization", "Bearer secret"), "***");
    EXPECT_EQ(redact::redact_header("Cookie", "session=abc"), "***");
}

TEST(SdkHelpersTest, RedactHeader_NonSensitiveKey_PassThrough) {
    EXPECT_EQ(redact::redact_header("Host", "example.com"), "example.com");
    EXPECT_EQ(redact::redact_header("Content-Type", "application/json"), "application/json");
}

// ===========================================================================
//  key character predicates
// ===========================================================================

TEST(SdkHelpersTest, KeyStartChar) {
    EXPECT_TRUE(redact::is_key_start_char('a'));
    EXPECT_TRUE(redact::is_key_start_char('Z'));
    EXPECT_TRUE(redact::is_key_start_char('_'));
    EXPECT_FALSE(redact::is_key_start_char('0'));
    EXPECT_FALSE(redact::is_key_start_char('-'));
    EXPECT_FALSE(redact::is_key_start_char('.'));
    EXPECT_FALSE(redact::is_key_start_char(':'));
}

TEST(SdkHelpersTest, KeyChar) {
    EXPECT_TRUE(redact::is_key_char('a'));
    EXPECT_TRUE(redact::is_key_char('9'));
    EXPECT_TRUE(redact::is_key_char('.'));
    EXPECT_TRUE(redact::is_key_char('-'));
    EXPECT_TRUE(redact::is_key_char('_'));
    EXPECT_FALSE(redact::is_key_char(':'));
    EXPECT_FALSE(redact::is_key_char('@'));
    EXPECT_FALSE(redact::is_key_char('/'));
}

TEST(SdkHelpersTest, KeyPosition) {
    EXPECT_TRUE(redact::is_key_position("token=a", 0));
    EXPECT_TRUE(redact::is_key_position("a&token=b", 2));
    EXPECT_TRUE(redact::is_key_position("a,token=b", 2));
    EXPECT_TRUE(redact::is_key_position("a token=b", 2));
    EXPECT_TRUE(redact::is_key_position("a\ntoken=b", 2));
    EXPECT_TRUE(redact::is_key_position("a\rtoken=b", 2));
    EXPECT_FALSE(redact::is_key_position("atoken=b", 1));  // 'a' is not a separator
    EXPECT_FALSE(redact::is_key_position("=token=b", 1));  // '=' is not a separator
}

// ===========================================================================
//  redact_body — form shape
// ===========================================================================

TEST(SdkHelpersTest, RedactBody_FormSingleParam) {
    EXPECT_EQ(redact::redact_body("token=abc"), "token=***");
}

TEST(SdkHelpersTest, RedactBody_FormMultipleParams) {
    EXPECT_EQ(redact::redact_body("token=abc&host=example.com"), "token=***&host=example.com");
}

TEST(SdkHelpersTest, RedactBody_FormNonSensitivePassThrough) {
    EXPECT_EQ(redact::redact_body("host=example.com&port=80"), "host=example.com&port=80");
}

TEST(SdkHelpersTest, RedactBody_FormSuffixKey) {
    EXPECT_EQ(redact::redact_body("my_secret=abc"), "my_secret=***");
}

TEST(SdkHelpersTest, RedactBody_Form_ValueTerminators) {
    // Every value terminator must stop the redacted span: '&', ',', '}', '\n', '\r'.
    EXPECT_EQ(redact::redact_body("token=abc&next=1"), "token=***&next=1");
    EXPECT_EQ(redact::redact_body("token=abc,next=1"), "token=***,next=1");
    EXPECT_EQ(redact::redact_body("token=abc}next"), "token=***}next");
    EXPECT_EQ(redact::redact_body("token=abc\nnext=1"), "token=***\nnext=1");
    EXPECT_EQ(redact::redact_body("token=abc\r\nnext=1"), "token=***\r\nnext=1");
}

TEST(SdkHelpersTest, RedactBody_Form_TrailingValue) {
    // No terminator at the end of input.
    EXPECT_EQ(redact::redact_body("token=abc"), "token=***");
}

// ===========================================================================
//  redact_body — JSON shape
// ===========================================================================

TEST(SdkHelpersTest, RedactBody_JsonSensitiveKey) {
    EXPECT_EQ(redact::redact_body(R"({"token": "abc"})"), R"({"token": ***})");
}

TEST(SdkHelpersTest, RedactBody_JsonSpaceAroundColon) {
    EXPECT_EQ(redact::redact_body(R"({"token" : "abc"})"), R"({"token" : ***})");
}

TEST(SdkHelpersTest, RedactBody_JsonMultipleSpacesBeforeValue) {
    EXPECT_EQ(redact::redact_body(R"({"token":   "abc"})"), R"({"token":   ***})");
}

TEST(SdkHelpersTest, RedactBody_JsonCommaSeparated) {
    EXPECT_EQ(redact::redact_body(R"({"token":"a", "host":"h"})"), R"({"token":***, "host":"h"})");
}

TEST(SdkHelpersTest, RedactBody_JsonNonSensitivePassThrough) {
    EXPECT_EQ(redact::redact_body(R"({"host": "h", "port": 80})"), R"({"host": "h", "port": 80})");
}

TEST(SdkHelpersTest, RedactBody_JsonMixed) {
    EXPECT_EQ(redact::redact_body(R"({"host":"h","password":"p","ttl":60})"),
              R"({"host":"h","password":***,"ttl":60})");
}

TEST(SdkHelpersTest, RedactBody_JsonUnterminatedKey_AppendsRest) {
    EXPECT_EQ(redact::redact_body(R"({"unterminated)"), R"({"unterminated)");
}

TEST(SdkHelpersTest, RedactBody_QuoteWithoutColon_PassThrough) {
    EXPECT_EQ(redact::redact_body(R"("hostname" then text)"), R"("hostname" then text)");
}

// ===========================================================================
//  redact_url_query
// ===========================================================================

TEST(SdkHelpersTest, RedactUrlQuery_NoQuery_PassThrough) {
    EXPECT_EQ(redact::redact_url_query("https://example.com/update"), "https://example.com/update");
}

TEST(SdkHelpersTest, RedactUrlQuery_WithSensitiveQuery) {
    EXPECT_EQ(redact::redact_url_query("https://example.com/update?token=abc&host=h"),
              "https://example.com/update?token=***&host=h");
}

TEST(SdkHelpersTest, RedactUrlQuery_EmptyQuery) {
    EXPECT_EQ(redact::redact_url_query("https://example.com/update?"), "https://example.com/update?");
}

// ===========================================================================
//  format_request
// ===========================================================================

namespace {
[[nodiscard]] yaddnsc::sdk::HttpRequest make_request(yaddnsc::sdk::Method method,
                                                     std::optional<std::string> body = std::nullopt) {
    yaddnsc::sdk::HttpRequest req;
    req.content_type = "application/json";
    req.method = method;
    req.url = "https://example.com/update";
    req.headers.push_back({"Host", "example.com"});
    req.headers.push_back({"Authorization", "Bearer top-secret"});
    req.body = std::move(body);
    return req;
}
}  // namespace

TEST(SdkHelpersTest, Format_AllMethods) {
    // Every Method maps to its canonical string.
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::Get)).find(R"(method="GET")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::Post)).find(R"(method="POST")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::Put)).find(R"(method="PUT")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::Patch)).find(R"(method="PATCH")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::Delete)).find(R"(method="DELETE")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::Head)).find(R"(method="HEAD")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::Options)).find(R"(method="OPTIONS")") !=
                std::string::npos);
}

TEST(SdkHelpersTest, Format_RedactsSensitiveHeader) {
    const auto out = yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::Post, "{}"));
    EXPECT_TRUE(out.find("Authorization=***") != std::string::npos);
    EXPECT_TRUE(out.find("Bearer top-secret") == std::string::npos);
    // Non-sensitive headers pass through.
    EXPECT_TRUE(out.find("Host=example.com") != std::string::npos);
}

TEST(SdkHelpersTest, Format_RedactsSensitiveBody) {
    const auto out = yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::Post, R"({"token": "abc"})"));
    EXPECT_TRUE(out.find(R"("token": ***)") != std::string::npos);
    EXPECT_TRUE(out.find("abc") == std::string::npos);
}

TEST(SdkHelpersTest, Format_EmptyBody) {
    const auto out = yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::Get));
    EXPECT_TRUE(out.find(R"(body="")") != std::string::npos);
}

// ===========================================================================
//  form encoding
// ===========================================================================

TEST(SdkHelpersTest, FormEncodeComponent_EncodesReservedAndSpace) {
    EXPECT_EQ(yaddnsc::sdk::encode_form_component("a b&c=d"), "a+b%26c%3Dd");
    EXPECT_EQ(yaddnsc::sdk::encode_form_component("~.-_"), "~.-_");
    EXPECT_EQ(yaddnsc::sdk::encode_form_component("100%"), "100%25");
}

TEST(SdkHelpersTest, FormEncodeComponent_EncodesUtf8Bytes) {
    // 'é' = U+00E9 → UTF-8 0xC3 0xA9.
    EXPECT_EQ(yaddnsc::sdk::encode_form_component("café"), "caf%C3%A9");
}

TEST(SdkHelpersTest, FormEncode_Form_JoinsPairs) {
    const std::multimap<std::string, std::string> params{{"k1", "v 1"}, {"k2", "v&2"}};
    EXPECT_EQ(yaddnsc::sdk::encode_form(params), "k1=v+1&k2=v%262");
}
