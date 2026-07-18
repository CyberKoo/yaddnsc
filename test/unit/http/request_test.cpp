//
// Unit tests for src/http/request.cpp — HTTP/1.1 request building.
//
// Verifies wire-format output for various method / header / body
// combinations, including edge cases like DEL→DELETE mapping,
// Host deduplication, and empty body handling.
// =============================================================================

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "http/request.h"
#include "fmt.hpp"

namespace {

/// Check that the request bytes contain @p substring (after converting both to
/// string_view for readable assertion output).
::testing::AssertionResult contains(std::string_view wire, std::string_view expected) {
    if (wire.find(expected) != std::string_view::npos)
        return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "Expected wire to contain \"" << expected << "\"";
}

/// Check that the request bytes do NOT contain @p substring.
::testing::AssertionResult not_contains(std::string_view wire, std::string_view expected) {
    if (wire.find(expected) == std::string_view::npos)
        return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "Expected wire NOT to contain \"" << expected << "\"";
}

} // anonymous namespace

// ── Method line ───────────────────────────────────────────────────────────

TEST(HttpRequestTest, GetMethod_RequestLine) {
    HttpRequest req;
    req.method = HttpMethod::GET;

    auto wire = Http::build_request(req, "/path", "example.com", "test-agent/1.0");
    std::string_view sv(reinterpret_cast<const char *>(wire.data()), wire.size());

    EXPECT_TRUE(contains(sv, "GET /path HTTP/1.1\r\n"));
    EXPECT_TRUE(contains(sv, "Host: example.com\r\n"));
    EXPECT_TRUE(contains(sv, "User-Agent: test-agent/1.0\r\n"));
    EXPECT_TRUE(not_contains(sv, "Content-Type"));
    EXPECT_TRUE(not_contains(sv, "Content-Length"));
}

TEST(HttpRequestTest, PostMethod_WithBody_IncludesContentHeaders) {
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.content_type = "application/json";
    req.body = R"({"key":"value"})";

    auto wire = Http::build_request(req, "/api", "api.example.com", "agent/1");
    std::string_view sv(reinterpret_cast<const char *>(wire.data()), wire.size());

    EXPECT_TRUE(contains(sv, "POST /api HTTP/1.1\r\n"));
    EXPECT_TRUE(contains(sv, "Content-Type: application/json\r\n"));
    EXPECT_TRUE(contains(sv, "Content-Length: 15\r\n"));
    // Body should appear after headers.
    EXPECT_TRUE(contains(sv, "\r\n\r\n{\"key\":\"value\"}"));
}

TEST(HttpRequestTest, DelMethod_WireFormatIsDelete) {
    HttpRequest req;
    req.method = HttpMethod::DEL;

    auto wire = Http::build_request(req, "/resource/42", "srv.net", "agent/1");
    std::string_view sv(reinterpret_cast<const char *>(wire.data()), wire.size());

    EXPECT_TRUE(contains(sv, "DELETE /resource/42 HTTP/1.1\r\n"));
}

// ── Host header ───────────────────────────────────────────────────────────

TEST(HttpRequestTest, HostInCustomHeaders_SkipsDefaultHost) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.headers.emplace("Host", "custom-host.net");

    // host_header param should NOT appear since Host is already in req.headers.
    auto wire = Http::build_request(req, "/", "fallback-host.net", "agent/1");
    std::string_view sv(reinterpret_cast<const char *>(wire.data()), wire.size());

    EXPECT_TRUE(contains(sv, "Host: custom-host.net\r\n")) << "wire=[" << sv << "]";
    EXPECT_TRUE(contains(sv, "Host: custom-host.net\r\n"));
    EXPECT_TRUE(not_contains(sv, "Host: fallback-host.net"));
}

TEST(HttpRequestTest, HostHeaderCaseInsensitive) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.headers.emplace("host", "lowercase-host.net");

    auto wire = Http::build_request(req, "/", "fallback.net", "agent/1");
    std::string_view sv(reinterpret_cast<const char *>(wire.data()), wire.size());

    // Check that custom Host appears and fallback does not.
    EXPECT_TRUE(contains(sv, "host: lowercase-host.net\r\n"));
    EXPECT_TRUE(not_contains(sv, "Host: fallback.net"));
}

// ── Body edge cases ───────────────────────────────────────────────────────

TEST(HttpRequestTest, NullBody_NoContentHeaders) {
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.content_type = "text/plain";
    // body is nullopt by default.

    auto wire = Http::build_request(req, "/", "h.net", "agent/1");
    std::string_view sv(reinterpret_cast<const char *>(wire.data()), wire.size());

    EXPECT_TRUE(not_contains(sv, "Content-Type"));
    EXPECT_TRUE(not_contains(sv, "Content-Length"));
}

TEST(HttpRequestTest, EmptyBody_NoContentHeaders) {
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.content_type = "text/plain";
    req.body = "";  // empty string

    auto wire = Http::build_request(req, "/", "h.net", "agent/1");
    std::string_view sv(reinterpret_cast<const char *>(wire.data()), wire.size());

    EXPECT_TRUE(not_contains(sv, "Content-Type"));
    EXPECT_TRUE(not_contains(sv, "Content-Length"));
}

TEST(HttpRequestTest, BodyWithoutContentType_OmitsContentType) {
    HttpRequest req;
    req.method = HttpMethod::PUT;
    req.body = "raw data";
    // content_type intentionally left empty.

    auto wire = Http::build_request(req, "/file", "h.net", "agent/1");
    std::string_view sv(reinterpret_cast<const char *>(wire.data()), wire.size());

    EXPECT_TRUE(contains(sv, "Content-Length: 8\r\n"));
    EXPECT_TRUE(not_contains(sv, "Content-Type"));
}

// ── Custom headers ────────────────────────────────────────────────────────

TEST(HttpRequestTest, CustomHeaders_AppearAfterStandardOnes) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.headers.emplace("X-Custom", "value1");
    req.headers.emplace("Accept", "application/json");

    auto wire = Http::build_request(req, "/", "h.net", "agent/1");
    std::string_view sv(reinterpret_cast<const char *>(wire.data()), wire.size());

    EXPECT_TRUE(contains(sv, "Accept: application/json\r\n"));
    EXPECT_TRUE(contains(sv, "X-Custom: value1\r\n"));
}

// ── CRLF injection protection ────────────────────────────────────────────

TEST(HttpRequestTest, PathCrlf_IsStripped) {
    // CRLF in the path must not produce an injected header line.
    HttpRequest req;
    req.method = HttpMethod::GET;

    auto wire = Http::build_request(req, "/dns-query\r\nX-Injected: true", "h.net", "a/1");
    std::string_view sv(reinterpret_cast<const char*>(wire.data()), wire.size());

    // The injected header line must NOT appear (no "\r\nX-Injected:").
    EXPECT_TRUE(not_contains(sv, "\r\nX-Injected:"));
    // CRLF was stripped, so "X-Injected" merges into the request line.
    EXPECT_TRUE(contains(sv, "GET /dns-queryX-Injected: true HTTP/1.1\r\n"));
}

TEST(HttpRequestTest, HeaderValueCrlf_IsStripped) {
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.headers.emplace("X-Custom", "safe\r\nX-Injected: evil");

    auto wire = Http::build_request(req, "/", "h.net", "a/1");
    std::string_view sv(reinterpret_cast<const char*>(wire.data()), wire.size());

    // No injected header line appears.
    EXPECT_TRUE(not_contains(sv, "\r\nX-Injected:"));
    // The sanitized value is part of the same header line.
    EXPECT_TRUE(contains(sv, "X-Custom: safeX-Injected: evil\r\n"));
}

TEST(HttpRequestTest, HeaderNameCrlf_IsStripped) {
    // CRLF in a header name must not allow injecting a separate header line.
    HttpRequest req;
    req.method = HttpMethod::GET;
    req.headers.emplace("Safe\r\nEvil: injected", "value");

    auto wire = Http::build_request(req, "/", "h.net", "a/1");
    std::string_view sv(reinterpret_cast<const char*>(wire.data()), wire.size());

    // "Evil: injected" should NOT appear as a header line (i.e. at start of line).
    EXPECT_TRUE(not_contains(sv, "\r\nEvil: injected"));
    // After stripping CRLF, the header name becomes "SafeEvil: injected".
    EXPECT_TRUE(contains(sv, "SafeEvil: injected: value\r\n"));
}

TEST(HttpRequestTest, CrlfInHeaderName_DoesNotBypassHostDedup) {
    // Even with CRLF in the header name, Host dedup must still work.
    HttpRequest req;
    req.method = HttpMethod::GET;
    // After CRLF stripping, "Ho\rst" becomes "Host" which should match.
    req.headers.emplace("Ho\rst", "custom-host.net");

    auto wire = Http::build_request(req, "/", "fallback.net", "a/1");
    std::string_view sv(reinterpret_cast<const char*>(wire.data()), wire.size());

    // Custom Host value should appear; fallback must NOT appear.
    EXPECT_TRUE(contains(sv, "Host: custom-host.net\r\n"));
    EXPECT_TRUE(not_contains(sv, "Host: fallback.net"));
}

TEST(HttpRequestTest, BinaryBody_WithCrLfBytes_NotSanitized) {
    // Binary DNS body may legitimately contain 0x0A / 0x0D.
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.body = std::string("\x00\x0A\x0D\xFF", 4);

    auto wire = Http::build_request(req, "/", "h.net", "a/1");
    std::string_view sv(reinterpret_cast<const char*>(wire.data()), wire.size());

    // Body must appear after \r\n\r\n with the original bytes preserved.
    EXPECT_TRUE(sv.find("\x00\x0A\x0D\xFF") != std::string_view::npos);
}

TEST(HttpRequestTest, ContentTypeCrlf_IsStripped) {
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.content_type = "text/plain\r\nX-Injected: true";
    req.body = "data";

    auto wire = Http::build_request(req, "/", "h.net", "a/1");
    std::string_view sv(reinterpret_cast<const char*>(wire.data()), wire.size());

    EXPECT_TRUE(not_contains(sv, "\r\nX-Injected:"));
    EXPECT_TRUE(contains(sv, "Content-Type: text/plainX-Injected: true\r\n"));
}

TEST(HttpRequestTest, HostHeaderParamCrlf_IsStripped) {
    HttpRequest req;
    req.method = HttpMethod::GET;

    auto wire = Http::build_request(req, "/", "h.net\r\nX-Injected: true", "a/1");
    std::string_view sv(reinterpret_cast<const char*>(wire.data()), wire.size());

    EXPECT_TRUE(not_contains(sv, "\r\nX-Injected:"));
    EXPECT_TRUE(contains(sv, "Host: h.netX-Injected: true\r\n"));
}

TEST(HttpRequestTest, UserAgentCrlf_IsStripped) {
    HttpRequest req;
    req.method = HttpMethod::GET;

    auto wire = Http::build_request(req, "/", "h.net", "agent/1\r\nX-Injected: true");
    std::string_view sv(reinterpret_cast<const char*>(wire.data()), wire.size());

    EXPECT_TRUE(not_contains(sv, "\r\nX-Injected:"));
    EXPECT_TRUE(contains(sv, "User-Agent: agent/1X-Injected: true\r\n"));
}
