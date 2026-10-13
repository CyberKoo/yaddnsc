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

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <format>
#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/form_encode.hpp>
#include <yaddnsc/sdk/redact.hpp>

namespace redact = yaddnsc::sdk::redact;

namespace {

struct MalformedResponseHost {
    yaddnsc_status status{YADDNSC_STATUS_OK};
    yaddnsc_http_response response{};
    yaddnsc_error error{};

    MalformedResponseHost() {
        response.struct_size = static_cast<uint32_t>(sizeof(response));
        error.struct_size = static_cast<uint32_t>(sizeof(error));
    }

    [[nodiscard]] yaddnsc_host_services table() noexcept {
        return {
            .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_host_services)),
            .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
            .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
            .context = this,
            .log = &log,
            .http_exchange = &exchange,
            .is_cancelled = &is_cancelled,
        };
    }

private:
    static void log(void*, yaddnsc_log_level, yaddnsc_string, const yaddnsc_source_location*) noexcept {}

    static yaddnsc_status exchange(void* context, const yaddnsc_http_request*, yaddnsc_http_response* out_response,
                                   yaddnsc_error* out_error) noexcept {
        const auto& self = *static_cast<MalformedResponseHost*>(context);
        *out_response = self.response;
        *out_error = self.error;
        return self.status;
    }

    static int32_t is_cancelled(void*) noexcept { return 0; }
};

[[nodiscard]] yaddnsc::sdk::HttpRequest exchange_request() {
    return {
        .method = yaddnsc::sdk::Method::GET,
        .url = "https://example.com",
        .headers = {},
        .body = std::nullopt,
        .content_type = {},
    };
}

void expect_contract_failure(const yaddnsc::sdk::ExchangeResult& result) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_INTERNAL_ERROR);
}

}  // namespace

TEST(SdkHelpersTest, ServicesRejectsMalformedHostResponseViews) {
    MalformedResponseHost host;
    const auto table = host.table();
    const yaddnsc::sdk::Services services(&table);

    host.response.body = {nullptr, 1};
    expect_contract_failure(services.exchange(exchange_request()));

    host.response.body = {};
    host.response.headers = nullptr;
    host.response.header_count = 1;
    expect_contract_failure(services.exchange(exchange_request()));

    std::array<yaddnsc_http_header, 1> headers{};
    headers[0] = {{nullptr, 1}, {"value", 5}};
    host.response.headers = headers.data();
    host.response.header_count = headers.size();
    expect_contract_failure(services.exchange(exchange_request()));

    headers[0] = {{"name", 4}, {nullptr, 1}};
    expect_contract_failure(services.exchange(exchange_request()));

    host.response.headers = nullptr;
    host.response.header_count = 0;
    host.response.struct_size = YADDNSC_HTTP_RESPONSE_MIN_SIZE - 1;
    expect_contract_failure(services.exchange(exchange_request()));
}

TEST(SdkHelpersTest, ServicesRejectsMalformedHostErrorReports) {
    MalformedResponseHost host;
    const auto table = host.table();
    const yaddnsc::sdk::Services services(&table);

    host.status = YADDNSC_STATUS_NETWORK_ERROR;
    host.error.status = YADDNSC_STATUS_NETWORK_ERROR;
    host.error.message = {nullptr, 1};
    expect_contract_failure(services.exchange(exchange_request()));

    host.error.message = {"network failed", sizeof("network failed") - 1};
    host.error.struct_size = YADDNSC_ERROR_MIN_SIZE - 1;
    expect_contract_failure(services.exchange(exchange_request()));

    host.error.struct_size = static_cast<uint32_t>(sizeof(host.error));
    host.error.status = YADDNSC_STATUS_CANCELLED;
    expect_contract_failure(services.exchange(exchange_request()));

    host.status = UINT32_C(99);
    host.error.status = UINT32_C(99);
    expect_contract_failure(services.exchange(exchange_request()));
}

struct ThrowingFormatArg {};

template<>
struct std::formatter<ThrowingFormatArg, char> {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    template<class FormatContext>
    auto format(const ThrowingFormatArg&, FormatContext& ctx) const {
        throw std::runtime_error("format failed");
        return ctx.out();
    }
};

class LongMessageDriver final : public yaddnsc::sdk::Driver {
public:
    yaddnsc::sdk::Result validate(std::string_view) const override { return {}; }

    LongMessageDriver() { throw std::runtime_error(std::string(800, 'x')); }

    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext&) override { return {}; }
};

class UpdateThrowDriver final : public yaddnsc::sdk::Driver {
public:
    yaddnsc::sdk::Result validate(std::string_view) const override { return {}; }

    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext&) override {
        throw std::runtime_error(std::string(800, 'y'));
    }
};

class LogThrowDriver final : public yaddnsc::sdk::Driver {
public:
    yaddnsc::sdk::Result validate(std::string_view) const override { return {}; }

    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext& context) override {
        YADDNSC_SDK_LOG_INFO(context, "{}", ThrowingFormatArg{});
        return {};
    }
};

struct StubHost {
    [[nodiscard]] yaddnsc_host_services table() noexcept {
        return {
            .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_host_services)),
            .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
            .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
            .context = this,
            .log = &log,
            .http_exchange = &exchange,
            .is_cancelled = &is_cancelled,
        };
    }

    static void log(void*, yaddnsc_log_level, yaddnsc_string, const yaddnsc_source_location*) noexcept {}

    static yaddnsc_status exchange(void*, const yaddnsc_http_request*, yaddnsc_http_response* out_response,
                                   yaddnsc_error*) noexcept {
        out_response->status_code = 204;
        out_response->headers = nullptr;
        out_response->header_count = 0;
        out_response->body = {};
        return YADDNSC_STATUS_OK;
    }

    static int32_t is_cancelled(void*) noexcept { return 0; }
};

struct BodyCaptureHost {
    yaddnsc_bytes seen{};

    [[nodiscard]] yaddnsc_host_services table() noexcept {
        return {
            .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_host_services)),
            .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
            .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
            .context = this,
            .log = &StubHost::log,
            .http_exchange = &exchange,
            .is_cancelled = &StubHost::is_cancelled,
        };
    }

    static yaddnsc_status exchange(void* context, const yaddnsc_http_request* request,
                                   yaddnsc_http_response* out_response, yaddnsc_error*) noexcept {
        static_cast<BodyCaptureHost*>(context)->seen = request->body;
        out_response->status_code = 204;
        out_response->headers = nullptr;
        out_response->header_count = 0;
        out_response->body = {};
        return YADDNSC_STATUS_OK;
    }
};

[[nodiscard]] yaddnsc_update_request blank_update_request() {
    yaddnsc_update_request request{};
    request.struct_size = static_cast<uint32_t>(sizeof(request));
    return request;
}

TEST(SdkHelpersTest, CreateExceptionMessageIsTruncatedWithoutThrowing) {
    StubHost host;
    const auto table = host.table();
    yaddnsc_driver* handle = nullptr;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));

    const auto status = yaddnsc::sdk::detail::create_driver<LongMessageDriver>(&table, &handle, &error);
    EXPECT_EQ(status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(handle, nullptr);
    EXPECT_EQ(error.status, YADDNSC_STATUS_INTERNAL_ERROR);
    ASSERT_NE(error.message.data, nullptr);
    EXPECT_EQ(error.message.size, 511u);
    EXPECT_EQ(std::string(error.message.data, error.message.size), std::string(511, 'x'));
}

TEST(SdkHelpersTest, UpdateExceptionMessageIsTruncatedWithoutThrowing) {
    StubHost host;
    const auto table = host.table();
    yaddnsc_driver* handle = nullptr;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    ASSERT_EQ(yaddnsc::sdk::detail::create_driver<UpdateThrowDriver>(&table, &handle, &error), YADDNSC_STATUS_OK);
    ASSERT_NE(handle, nullptr);

    error = {};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    const auto request = blank_update_request();
    const auto status = yaddnsc::sdk::detail::update_driver(handle, &request, &error);
    EXPECT_EQ(status, YADDNSC_STATUS_INTERNAL_ERROR);
    EXPECT_EQ(error.status, YADDNSC_STATUS_INTERNAL_ERROR);
    ASSERT_NE(error.message.data, nullptr);
    EXPECT_EQ(error.message.size, 511u);
    EXPECT_EQ(std::string(error.message.data, error.message.size), std::string(511, 'y'));

    yaddnsc::sdk::detail::destroy_driver(handle);
}

TEST(SdkHelpersTest, LogFormatFailureDoesNotFailUpdate) {
    StubHost host;
    const auto table = host.table();
    yaddnsc_driver* handle = nullptr;
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    ASSERT_EQ(yaddnsc::sdk::detail::create_driver<LogThrowDriver>(&table, &handle, &error), YADDNSC_STATUS_OK);

    error = {};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    const auto request = blank_update_request();
    EXPECT_EQ(yaddnsc::sdk::detail::update_driver(handle, &request, &error), YADDNSC_STATUS_OK);

    yaddnsc::sdk::detail::destroy_driver(handle);
}

TEST(SdkHelpersTest, ExchangeDistinguishesAbsentAndEmptyBody) {
    BodyCaptureHost host;
    const auto table = host.table();
    const yaddnsc::sdk::Services services(&table);

    yaddnsc::sdk::HttpRequest absent;
    absent.url = "https://example.com";
    absent.body = std::nullopt;
    ASSERT_TRUE(services.exchange(absent).has_value());
    EXPECT_EQ(host.seen.data, nullptr);
    EXPECT_EQ(host.seen.size, 0u);

    yaddnsc::sdk::HttpRequest empty;
    empty.url = "https://example.com";
    empty.body = std::string{};
    ASSERT_TRUE(services.exchange(empty).has_value());
    EXPECT_NE(host.seen.data, nullptr);
    EXPECT_EQ(host.seen.size, 0u);
}

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
    EXPECT_EQ(redact::redact_body("token=abc,next=1"), "token=***");
    EXPECT_EQ(redact::redact_body("token=abc}next"), "token=***");
    EXPECT_EQ(redact::redact_body("token=abc\nnext=1"), "token=***");
    EXPECT_EQ(redact::redact_body("token=abc\r\nnext=1"), "token=***");
}

TEST(SdkHelpersTest, RedactBody_Form_TrailingValue) {
    // No terminator at the end of input.
    EXPECT_EQ(redact::redact_body("token=abc"), "token=***");
}

// ===========================================================================
//  redact_body — JSON shape
// ===========================================================================

TEST(SdkHelpersTest, RedactBody_JsonSensitiveKey) {
    EXPECT_EQ(redact::redact_body(R"({"token": "abc"})"), R"({"token":"***"})");
}

TEST(SdkHelpersTest, RedactBody_JsonSpaceAroundColon) {
    EXPECT_EQ(redact::redact_body(R"({"token" : "abc"})"), R"({"token":"***"})");
}

TEST(SdkHelpersTest, RedactBody_JsonMultipleSpacesBeforeValue) {
    EXPECT_EQ(redact::redact_body(R"({"token":   "abc"})"), R"({"token":"***"})");
}

TEST(SdkHelpersTest, RedactBody_JsonCommaSeparated) {
    EXPECT_EQ(redact::redact_body(R"({"token":"a", "host":"h"})"), R"({"token":"***","host":"h"})");
}

TEST(SdkHelpersTest, RedactBody_JsonNonSensitivePassThrough) {
    EXPECT_EQ(redact::redact_body(R"({"host": "h", "port": 80})"), R"({"host":"h","port":80})");
}

TEST(SdkHelpersTest, RedactBody_JsonMixed) {
    EXPECT_EQ(redact::redact_body(R"({"host":"h","password":"p","ttl":60})"),
              R"({"host":"h","password":"***","ttl":60})");
}

TEST(SdkHelpersTest, RedactBody_JsonUnterminatedKey_HidesContent) {
    EXPECT_EQ(redact::redact_body(R"({"unterminated)"), "***");
}

TEST(SdkHelpersTest, RedactBody_QuoteWithoutColon_HidesContent) {
    EXPECT_EQ(redact::redact_body(R"("hostname" then text)"), "***");
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
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::GET)).find(R"(method="GET")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::POST)).find(R"(method="POST")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::PUT)).find(R"(method="PUT")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::PATCH)).find(R"(method="PATCH")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::DELETE)).find(R"(method="DELETE")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::HEAD)).find(R"(method="HEAD")") !=
                std::string::npos);
    EXPECT_TRUE(yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::OPTIONS)).find(R"(method="OPTIONS")") !=
                std::string::npos);
}

TEST(SdkHelpersTest, Format_RedactsSensitiveHeader) {
    const auto out = yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::POST, "{}"));
    EXPECT_TRUE(out.find("Authorization=***") != std::string::npos);
    EXPECT_TRUE(out.find("Bearer top-secret") == std::string::npos);
    // Non-sensitive headers pass through.
    EXPECT_TRUE(out.find("Host=example.com") != std::string::npos);
}

TEST(SdkHelpersTest, Format_RedactsSensitiveBody) {
    const auto out = yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::POST, R"({"token": "abc"})"));
    EXPECT_TRUE(out.find(R"("token":"***")") != std::string::npos);
    EXPECT_TRUE(out.find("abc") == std::string::npos);
}

TEST(SdkHelpersTest, Format_EmptyBody) {
    const auto out = yaddnsc::sdk::format_request(make_request(yaddnsc::sdk::Method::GET));
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

namespace {

using yaddnsc::sdk::HttpHeaderView;
using yaddnsc::sdk::HttpResponse;
using yaddnsc::sdk::detail::classify_upstream_failure;
using yaddnsc::sdk::detail::retry_after_seconds;

HttpResponse response_with(uint32_t status, std::initializer_list<HttpHeaderView> headers) {
    return HttpResponse{.status_code = status, .headers = {headers.begin(), headers.end()}, .body = {}};
}

}  // namespace

TEST(SdkHelpersTest, ClassifyUpstreamFailure_AuthAndRateLimit) {
    EXPECT_EQ(classify_upstream_failure(response_with(401, {})).status, YADDNSC_STATUS_AUTHENTICATION_FAILED);
    EXPECT_EQ(classify_upstream_failure(response_with(403, {})).status, YADDNSC_STATUS_AUTHENTICATION_FAILED);
    EXPECT_EQ(classify_upstream_failure(response_with(400, {})).status, YADDNSC_STATUS_UPSTREAM_REJECTED);
    EXPECT_EQ(classify_upstream_failure(response_with(500, {})).status, YADDNSC_STATUS_UPSTREAM_REJECTED);

    const auto limited = classify_upstream_failure(response_with(429, {{"retry-after", "45"}}));
    EXPECT_EQ(limited.status, YADDNSC_STATUS_RATE_LIMITED);
    EXPECT_EQ(limited.retry_after_seconds, 45u);

    const auto auth_with_hint = classify_upstream_failure(response_with(401, {{"Retry-After", "30"}}));
    EXPECT_EQ(auth_with_hint.status, YADDNSC_STATUS_AUTHENTICATION_FAILED);
    EXPECT_EQ(auth_with_hint.retry_after_seconds, 0u);
}

TEST(SdkHelpersTest, RetryAfter_DelaySecondsAndHttpDate) {
    EXPECT_EQ(retry_after_seconds(response_with(429, {{"Retry-After", "  12  "}})), 12u);
    EXPECT_EQ(retry_after_seconds(response_with(429, {{"Retry-After", "99999999999"}})),
              std::numeric_limits<uint32_t>::max());
    EXPECT_EQ(retry_after_seconds(response_with(429, {{"Retry-After", "soon"}, {"Retry-After", "30"}})), 0u);
    EXPECT_EQ(retry_after_seconds(response_with(429, {{"Retry-After", "Sun, 06 Nov 1994 08:49:37 GMT"}})), 0u);

    const auto future = retry_after_seconds(response_with(429, {{"Retry-After", "Tue, 01 Jan 2036 00:00:00 GMT"}}));
    EXPECT_GT(future, 0u);
    EXPECT_LT(future, std::numeric_limits<uint32_t>::max());
}

TEST(SdkHelpersTest, RedactJsonParsesSecretsWithCommasEscapesNestedObjectsAndArrays) {
    const auto output = redact::redact_body(
        R"({"token":"SYNTHETIC_A,SYNTHETIC_B","nested":[{"pass\u0077ord":{"value":"SECRET"},"host":"safe"}],"secretapikey":"PORKBUN"})");
    EXPECT_EQ(output.find("SYNTHETIC"), std::string::npos);
    EXPECT_EQ(output.find("SECRET"), std::string::npos);
    EXPECT_EQ(output.find("PORKBUN"), std::string::npos);
    EXPECT_NE(output.find("safe"), std::string::npos);
    glz::generic parsed;
    EXPECT_FALSE(glz::read_json(parsed, output));
    EXPECT_EQ(redact::redact_body(R"({"token":"SECRET",)"), "***");
    EXPECT_EQ(redact::redact_body(R"({"host":"safe"} trailing SECRET)"), "***");
}

TEST(SdkHelpersTest, RedactFormDecodesKeysAndHidesMalformedContent) {
    EXPECT_EQ(redact::redact_body("%74oken=SECRET_A,SECRET_B&host=safe"), "%74oken=***&host=safe");
    EXPECT_EQ(redact::redact_body("token=SECRET&broken"), "***");
    EXPECT_EQ(redact::redact_body("to%XXken=SECRET"), "***");
    EXPECT_EQ(redact::redact_body("unstructured SECRET"), "***");
}

TEST(SdkHelpersTest, FormatRequestIncludesRedactedUrl) {
    yaddnsc::sdk::HttpRequest request;
    request.url = "https://example.com/update?token=SECRET_A,SECRET_B&host=safe";
    const auto output = yaddnsc::sdk::format_request(request);
    EXPECT_EQ(output.find("SECRET"), std::string::npos);
    EXPECT_NE(output.find("https://example.com/update?token=***&host=safe"), std::string::npos);
}

TEST(SdkHelpersTest, LoggingProtectsArgumentAndServiceEvaluation) {
    StubHost host;
    auto table = host.table();
    int evaluations = 0;
    const auto throwing_arg = [&]() -> int {
        ++evaluations;
        throw std::runtime_error("argument failed");
    };
    EXPECT_NO_THROW(YADDNSC_SDK_LOG_INFO(&table, "{}", throwing_arg()));
    EXPECT_EQ(evaluations, 1);
    table.log = nullptr;
    EXPECT_NO_THROW(YADDNSC_SDK_LOG_INFO(&table, "{}", throwing_arg()));
    EXPECT_NO_THROW(YADDNSC_SDK_LOG_INFO(static_cast<const yaddnsc_host_services*>(nullptr), "{}", throwing_arg()));
    EXPECT_EQ(evaluations, 1);
    const auto throwing_services = []() -> const yaddnsc_host_services* { throw std::runtime_error("lookup failed"); };
    EXPECT_NO_THROW(YADDNSC_SDK_LOG_INFO(throwing_services(), "{}", throwing_arg()));
    EXPECT_EQ(evaluations, 1);
}

TEST(SdkHelpersTest, NamedFormattingOnlyScansTemplateAndDefinesEscapingAndDuplicates) {
    using yaddnsc::sdk::fmt::arg;
    using yaddnsc::sdk::fmt::format;
    EXPECT_EQ(format("{a}|{b}", arg("a", "{b}"), arg("b", "VALUE")), "{b}|VALUE");
    EXPECT_EQ(format("{a}|{b}", arg("b", "VALUE"), arg("a", "{b}")), "{b}|VALUE");
    EXPECT_EQ(format("{{a}}|{unknown}|{a}|{unclosed", arg("a", "first"), arg("a", "last")),
              "{a}|{unknown}|last|{unclosed");
}

namespace {
template<typename T>
concept DescriptorString = requires(T& value) { yaddnsc::sdk::detail::descriptor_string(value); };
static_assert(DescriptorString<const char[4]>);
static_assert(!DescriptorString<const char*>);
constexpr char DESCRIPTOR_NAME[] = "abc";
constexpr auto DESCRIPTOR_VIEW = yaddnsc::sdk::detail::descriptor_string(DESCRIPTOR_NAME);
static_assert(DESCRIPTOR_VIEW.size == 3);

class MissingValidateDriver final : public yaddnsc::sdk::Driver {
public:
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext&) override { return {}; }
};

static_assert(std::is_abstract_v<MissingValidateDriver>);
}  // namespace

class InjectedServicesDriver final : public yaddnsc::sdk::Driver {
public:
    explicit InjectedServicesDriver(yaddnsc::sdk::Services services) {
        if (services.get() == nullptr)
            throw std::runtime_error("services not injected");
        YADDNSC_SDK_LOG_INFO(services, "Constructing driver");
    }

    yaddnsc::sdk::Result validate(std::string_view) const override { return {}; }

    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext&) override { return {}; }
};

TEST(SdkHelpersTest, CreateSupportsServicesInjectionAndCopiesOnlyKnownCapacity) {
    StubHost host;
    auto table = host.table();
    table.struct_size = YADDNSC_HOST_SERVICES_MIN_SIZE;
    yaddnsc_driver* driver = nullptr;
    EXPECT_EQ((yaddnsc::sdk::detail::create_driver<InjectedServicesDriver>(&table, &driver, nullptr)),
              YADDNSC_STATUS_OK);
    ASSERT_NE(driver, nullptr);
    yaddnsc::sdk::detail::destroy_driver(driver);
    table.struct_size = YADDNSC_HOST_SERVICES_MIN_SIZE - 1;
    EXPECT_EQ((yaddnsc::sdk::detail::create_driver<InjectedServicesDriver>(&table, &driver, nullptr)),
              YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(driver, nullptr);
}

class RunUpdateProbe : public yaddnsc::sdk::Driver {
public:
    using Driver::run_update;
};

TEST(SdkHelpersTest, RunUpdateAcceptsCapturingCheckerAndPreservesDetailedResult) {
    StubHost host;
    const auto table = host.table();
    yaddnsc::sdk::UpdateRequest parameters{};
    yaddnsc::sdk::UpdateContext context(parameters, &table);
    yaddnsc::sdk::HttpRequest request;
    request.url = "https://example.com";
    const std::string detail = "provider response missing record_id";
    auto result = RunUpdateProbe::run_update(
        context, "test", request,
        [&](const yaddnsc::sdk::HttpResponse&, const yaddnsc::sdk::Services&) -> yaddnsc::sdk::Result {
            return std::unexpected(yaddnsc::sdk::Error{YADDNSC_STATUS_INVALID_RESPONSE, detail, 17});
        });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().status, YADDNSC_STATUS_INVALID_RESPONSE);
    EXPECT_EQ(result.error().message, detail);
    EXPECT_EQ(result.error().retry_after_seconds, 17u);
}

TEST(SdkHelpersTest, RedactionUsesDeclaredMediaTypeAndHidesUnsupportedOrMalformedBodies) {
    EXPECT_EQ(redact::redact_body("token=secret", "application/json"), "***");
    EXPECT_EQ(redact::redact_body(R"({"token":"secret"})", "application/problem+json; charset=utf-8"),
              R"({"token":"***"})");
    EXPECT_EQ(redact::redact_body("host=safe&token=secret", "application/x-www-form-urlencoded"),
              "host=safe&token=***");
    EXPECT_EQ(redact::redact_body("opaque secret", "text/plain"), "***");
    EXPECT_EQ(redact::redact_body(R"("secret")", "application/json"), "***");
}
