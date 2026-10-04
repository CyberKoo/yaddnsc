//
// Unit tests for the shared request-building helpers
// (src/infrastructure/network/http/wire_request.cpp).
//
// persistent_client_test.cpp exercises these helpers indirectly through the
// client; this file drives them directly so the rejection paths and the
// host-owned header rules are asserted on their own:
//   - default_port / make_host_header   default-port elision, IPv6 bracketing
//   - make_target                       empty path, query handling
//   - validate_request                  header grammar and framing headers
//   - build_wire_request                managed-header override and injection
//   - to_public_request                 the inverse hop, incl. casing
//   - map_connect_error                 transport error classification
// =============================================================================

#include "infrastructure/network/http/wire_request.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/uri.h"

using net::http::ErrorCode;
using net::http::HttpVersion;
using net::http::Method;
using net::http::Options;
using net::http::Request;
using Transport::IoError;

namespace {

/// Uri has no public default constructor, so a parse failure aborts the test.
Uri parse_uri(std::string_view text) {
    auto parsed = Uri::parse(text);
    if (!parsed.has_value()) {
        ADD_FAILURE() << "failed to parse URI: " << text;
    }
    return *parsed;
}

/// ASCII case-insensitive header-name comparison (header names are tokens,
/// so folding with | 0x20 is enough and avoids locale tables).
bool ascii_iequals(std::string_view lhs, std::string_view rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) { return (a | 0x20) == (b | 0x20); });
}

std::string header_value(const net::http::protocol::WireRequest& wire, std::string_view name) {
    for (const auto& [key, value] : wire.headers) {
        if (ascii_iequals(key, name)) {
            return value;
        }
    }
    return {};
}

Request base_request() {
    Request req{.method = Method::GET};
    return req;
}

/// Case-insensitive lookup on a public Request (its headers are a multimap,
/// so this walks the keys instead of using find()).
std::string request_header(const Request& req, std::string_view name) {
    for (const auto& [key, value] : req.headers) {
        if (ascii_iequals(key, name)) {
            return value;
        }
    }
    return {};
}

// ===========================================================================
// default_port / make_host_header
// ===========================================================================

TEST(WireRequestHelpers, DefaultPort_httpsIs443AndEverythingElseIs80) {
    EXPECT_EQ(net::http::default_port("https"), 443);
    EXPECT_EQ(net::http::default_port("http"), 80);
    EXPECT_EQ(net::http::default_port(""), 80);
}

TEST(WireRequestHelpers, HostHeader_OmitsDefaultPortForMatchingScheme) {
    EXPECT_EQ(net::http::make_host_header("https", "example.com", 443), "example.com");
    EXPECT_EQ(net::http::make_host_header("http", "example.com", 80), "example.com");
}

TEST(WireRequestHelpers, HostHeader_AppendsNonDefaultPort) {
    EXPECT_EQ(net::http::make_host_header("https", "example.com", 8443), "example.com:8443");
    // Port 80 is not the https default, so it is still rendered.
    EXPECT_EQ(net::http::make_host_header("https", "example.com", 80), "example.com:80");
}

TEST(WireRequestHelpers, HostHeader_BracketsIpv6Literal) {
    EXPECT_EQ(net::http::make_host_header("https", "::1", 443), "[::1]");
    EXPECT_EQ(net::http::make_host_header("https", "2001:db8::1", 8443), "[2001:db8::1]:8443");
}

// ===========================================================================
// make_target
// ===========================================================================

TEST(WireRequestHelpers, Target_EmptyPathBecomesSlash) {
    EXPECT_EQ(net::http::make_target(parse_uri("http://example.com")), "/");
}

TEST(WireRequestHelpers, Target_JoinsPathAndQuery) {
    EXPECT_EQ(net::http::make_target(parse_uri("http://example.com/dns-query?dns=abc")), "/dns-query?dns=abc");
}

TEST(WireRequestHelpers, Target_PathWithoutQuery) {
    EXPECT_EQ(net::http::make_target(parse_uri("http://example.com/only-path")), "/only-path");
}

TEST(WireRequestHelpers, Target_EmptyQueryStringIsOmitted) {
    // A trailing '?' with nothing after it must not leave a bare '?' behind.
    EXPECT_EQ(net::http::make_target(parse_uri("http://example.com/p?")), "/p");
}

// ===========================================================================
// validate_request
// ===========================================================================

TEST(WireRequestValidate, EmptyHeaders_IsValid) {
    const auto result = net::http::validate_request(base_request());
    EXPECT_TRUE(result.has_value());
}

TEST(WireRequestValidate, InvalidHeaderName_IsRejected) {
    auto req = base_request();
    req.headers.emplace("Bad Name", "value");
    const auto result = net::http::validate_request(req);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::INVALID_REQUEST);
}

TEST(WireRequestValidate, HeaderValueWithCrLf_IsRejected) {
    auto req = base_request();
    req.headers.emplace("X-Thing", std::string("a\r\nb", 4));
    const auto result = net::http::validate_request(req);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::INVALID_REQUEST);
}

TEST(WireRequestValidate, UpgradeHeader_IsUnsupported) {
    auto req = base_request();
    req.headers.emplace("Upgrade", "websocket");
    const auto result = net::http::validate_request(req);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::UNSUPPORTED_PROTOCOL);
}

TEST(WireRequestValidate, TransferEncodingHeader_IsRejected) {
    auto req = base_request();
    req.headers.emplace("Transfer-Encoding", "chunked");
    const auto result = net::http::validate_request(req);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::INVALID_REQUEST);
}

TEST(WireRequestValidate, TrailerHeader_IsRejected) {
    auto req = base_request();
    req.headers.emplace("Trailer", "Expires");
    const auto result = net::http::validate_request(req);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::INVALID_REQUEST);
}

TEST(WireRequestValidate, InvalidContentType_IsRejected) {
    auto req = base_request();
    req.content_type = std::string("text/plain\r\nX-Injected: 1", 25);
    const auto result = net::http::validate_request(req);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::INVALID_REQUEST);
}

TEST(WireRequestValidate, ContentTypeCheckedEvenWithoutBody) {
    // The content_type check is unconditional, not gated on a body.
    auto req = base_request();
    req.content_type = std::string(1, '\x01');
    const auto result = net::http::validate_request(req);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::INVALID_REQUEST);
}

// ===========================================================================
// build_wire_request
// ===========================================================================

TEST(BuildWireRequest, InjectsHostHeader) {
    Options opts;
    const auto wire = net::http::build_wire_request(base_request(), "https", "example.com", 443, opts);
    EXPECT_EQ(header_value(wire, "Host"), "example.com");
    EXPECT_TRUE(wire.body == std::nullopt);
}

TEST(BuildWireRequest, UserManagedHeaders_AreDiscarded) {
    // This client owns framing, so a caller-supplied Host / Content-Length /
    // Content-Type / Connection / User-Agent must not survive.
    auto req = base_request();
    req.headers.emplace("host", "attacker.example");
    req.headers.emplace("content-length", "999");
    req.headers.emplace("content-type", "text/evil");
    req.headers.emplace("connection", "keep-alive, upgrade");
    req.headers.emplace("user-agent", "evil/1.0");
    req.headers.emplace("transfer-encoding", "chunked");
    req.headers.emplace("trailer", "Expires");
    req.headers.emplace("upgrade", "websocket");

    Options opts;
    const auto wire = net::http::build_wire_request(req, "https", "example.com", 443, opts);

    EXPECT_EQ(header_value(wire, "Host"), "example.com");
    // Exactly one Host, and it is the injected one — the caller's lowercase
    // "host" was discarded rather than merged.
    EXPECT_EQ(wire.headers.count("Host"), 1u);
    EXPECT_EQ(wire.headers.count("host"), 0u);
    EXPECT_EQ(wire.headers.count("content-length"), 0u);
    EXPECT_EQ(wire.headers.count("content-type"), 0u);
    EXPECT_EQ(wire.headers.count("user-agent"), 0u);
    EXPECT_EQ(wire.headers.count("transfer-encoding"), 0u);
    EXPECT_EQ(wire.headers.count("trailer"), 0u);
    EXPECT_EQ(wire.headers.count("upgrade"), 0u);
}

TEST(BuildWireRequest, UnmanagedHeaders_ArePreserved) {
    auto req = base_request();
    req.headers.emplace("X-Custom", "keep-me");
    req.headers.emplace("Accept", "application/dns-message");

    Options opts;
    const auto wire = net::http::build_wire_request(req, "https", "example.com", 443, opts);
    EXPECT_EQ(header_value(wire, "X-Custom"), "keep-me");
    EXPECT_EQ(header_value(wire, "Accept"), "application/dns-message");
}

TEST(BuildWireRequest, Http11WithoutKeepAlive_AddsConnectionClose) {
    Options opts;
    opts.version = HttpVersion::V1_1;
    opts.keep_alive = false;
    const auto wire = net::http::build_wire_request(base_request(), "https", "example.com", 443, opts);
    EXPECT_EQ(header_value(wire, "Connection"), "close");
}

TEST(BuildWireRequest, Http11WithKeepAlive_OmitsConnection) {
    Options opts;
    opts.version = HttpVersion::V1_1;
    opts.keep_alive = true;
    const auto wire = net::http::build_wire_request(base_request(), "https", "example.com", 443, opts);
    EXPECT_TRUE(header_value(wire, "Connection").empty());
}

TEST(BuildWireRequest, Http10WithKeepAlive_AddsConnectionKeepAlive) {
    Options opts;
    opts.version = HttpVersion::V1_0;
    opts.keep_alive = true;
    const auto wire = net::http::build_wire_request(base_request(), "https", "example.com", 443, opts);
    EXPECT_EQ(header_value(wire, "Connection"), "keep-alive");
    EXPECT_EQ(wire.version, HttpVersion::V1_0);
}

TEST(BuildWireRequest, Http10WithoutKeepAlive_AddsConnectionClose) {
    Options opts;
    opts.version = HttpVersion::V1_0;
    opts.keep_alive = false;
    const auto wire = net::http::build_wire_request(base_request(), "https", "example.com", 443, opts);
    EXPECT_EQ(header_value(wire, "Connection"), "close");
}

TEST(BuildWireRequest, UserAgent_IsInjectedWhenConfigured) {
    Options opts;
    opts.user_agent = "yaddnsc/1.0";
    const auto wire = net::http::build_wire_request(base_request(), "https", "example.com", 443, opts);
    EXPECT_EQ(header_value(wire, "User-Agent"), "yaddnsc/1.0");
}

TEST(BuildWireRequest, EmptyUserAgent_OmitsTheHeader) {
    Options opts;
    opts.user_agent = "";
    const auto wire = net::http::build_wire_request(base_request(), "https", "example.com", 443, opts);
    EXPECT_EQ(wire.headers.count("User-Agent"), 0u);
}

TEST(BuildWireRequest, Body_AddsContentLengthAndContentType) {
    auto req = base_request();
    req.method = Method::POST;
    req.set_body("hello");
    req.content_type = "application/dns-message";

    Options opts;
    const auto wire = net::http::build_wire_request(req, "https", "example.com", 443, opts);
    EXPECT_EQ(header_value(wire, "Content-Length"), "5");
    EXPECT_EQ(header_value(wire, "Content-Type"), "application/dns-message");
    ASSERT_TRUE(wire.body.has_value());
    EXPECT_EQ(*wire.body, "hello");
}

TEST(BuildWireRequest, BodyWithoutContentType_OmitsContentTypeHeader) {
    auto req = base_request();
    req.method = Method::POST;
    req.set_body("raw");

    Options opts;
    const auto wire = net::http::build_wire_request(req, "https", "example.com", 443, opts);
    EXPECT_EQ(header_value(wire, "Content-Length"), "3");
    EXPECT_EQ(wire.headers.count("Content-Type"), 0u);
}

TEST(BuildWireRequest, EmptyBody_StillCarriesZeroContentLength) {
    // An engaged but empty body means "send a body", so Content-Length: 0.
    auto req = base_request();
    req.method = Method::POST;
    req.set_body("");

    Options opts;
    const auto wire = net::http::build_wire_request(req, "https", "example.com", 443, opts);
    EXPECT_EQ(header_value(wire, "Content-Length"), "0");
    ASSERT_TRUE(wire.body.has_value());
    EXPECT_TRUE(wire.body->empty());
}

TEST(BuildWireRequest, MethodAndVersionAreCarriedThrough) {
    auto req = base_request();
    req.method = Method::DEL;
    Options opts;
    opts.version = HttpVersion::V1_0;
    const auto wire = net::http::build_wire_request(req, "https", "example.com", 443, opts);
    EXPECT_EQ(wire.method, Method::DEL);
    EXPECT_EQ(wire.version, HttpVersion::V1_0);
}

// ===========================================================================
// to_public_request
// ===========================================================================

TEST(ToPublicRequest, ManagedHeaders_AreDropped) {
    // The next build_wire_request re-derives framing for the new origin, so
    // nothing host-managed may be carried across the hop.
    net::http::protocol::WireRequest wire;
    wire.method = Method::GET;
    wire.headers.emplace("Host", "old.example");
    wire.headers.emplace("User-Agent", "yaddnsc/1.0");
    wire.headers.emplace("Content-Length", "0");
    wire.headers.emplace("Connection", "close");

    const auto req = net::http::to_public_request(wire);
    EXPECT_TRUE(req.headers.empty());
    EXPECT_TRUE(req.content_type.empty());
}

TEST(ToPublicRequest, ContentType_MovesToTheRequestField) {
    net::http::protocol::WireRequest wire;
    wire.method = Method::POST;
    wire.headers.emplace("Content-Type", "application/dns-message");

    const auto req = net::http::to_public_request(wire);
    EXPECT_EQ(req.content_type, "application/dns-message");
    EXPECT_TRUE(req.headers.empty());
}

TEST(ToPublicRequest, ManagedHeaders_AreDroppedRegardlessOfCasing) {
    // Regression: the per-file copy this replaced compared header names
    // case-sensitively, so a non-canonical casing would slip into the next
    // request instead of staying host-owned. Only the Content-Type value is
    // recoverable, and only because the case-insensitive match catches it.
    net::http::protocol::WireRequest wire;
    wire.method = Method::GET;
    wire.headers.emplace("host", "attacker.example");
    wire.headers.emplace("user-agent", "evil/1.0");
    wire.headers.emplace("content-length", "999");
    wire.headers.emplace("connection", "keep-alive, upgrade");
    wire.headers.emplace("CONTENT-TYPE", "text/evil");

    const auto req = net::http::to_public_request(wire);
    EXPECT_TRUE(req.headers.empty()) << "managed headers leaked across: " << req.headers.size();
    EXPECT_EQ(req.content_type, "text/evil");
}

TEST(ToPublicRequest, UnmanagedHeaders_ArePreserved) {
    net::http::protocol::WireRequest wire;
    wire.method = Method::GET;
    wire.headers.emplace("X-Custom", "keep-me");
    wire.headers.emplace("Authorization", "Bearer t");

    const auto req = net::http::to_public_request(wire);
    EXPECT_EQ(request_header(req, "x-custom"), "keep-me");
    EXPECT_EQ(request_header(req, "authorization"), "Bearer t");
    // The original name casing is carried through untouched.
    EXPECT_EQ(req.headers.count("X-Custom"), 1u);
    EXPECT_EQ(req.headers.count("Authorization"), 1u);
}

TEST(ToPublicRequest, RoundTripRebuildsFramingForTheNewOrigin) {
    auto req = base_request();
    req.method = Method::POST;
    req.set_body("payload");
    req.content_type = "application/dns-message";
    req.headers.emplace("X-Custom", "keep-me");
    // A caller-supplied Host never reaches the wire; assert the round trip
    // does not resurrect it.
    req.headers.emplace("host", "attacker.example");

    Options opts;
    const auto first = net::http::build_wire_request(req, "https", "old.example", 443, opts);
    const auto back = net::http::to_public_request(first);
    const auto second = net::http::build_wire_request(back, "https", "new.example", 443, opts);

    EXPECT_EQ(header_value(second, "Host"), "new.example");
    EXPECT_EQ(header_value(second, "Content-Type"), "application/dns-message");
    EXPECT_EQ(header_value(second, "Content-Length"), "7");
    EXPECT_EQ(header_value(second, "X-Custom"), "keep-me");
    EXPECT_EQ(second.headers.count("host"), 0u);
    ASSERT_TRUE(second.body.has_value());
    EXPECT_EQ(*second.body, "payload");
}

// ===========================================================================
// map_connect_error
// ===========================================================================

TEST(MapConnectError, Cancelled_MapsToCancelled) {
    EXPECT_EQ(net::http::map_connect_error(IoError::CANCELLED).code, ErrorCode::CANCELLED);
}

TEST(MapConnectError, Timeout_MapsToTimeout) {
    EXPECT_EQ(net::http::map_connect_error(IoError::TIMEOUT).code, ErrorCode::TIMEOUT);
}

TEST(MapConnectError, ConnectionFailed_MapsToConnectFailed) {
    EXPECT_EQ(net::http::map_connect_error(IoError::CONNECTION_FAILED).code, ErrorCode::CONNECT_FAILED);
}

TEST(MapConnectError, CarriesContextualMessage) {
    const auto err = net::http::map_connect_error(IoError::TIMEOUT);
    EXPECT_NE(std::string_view(err.message).find("connect/handshake"), std::string_view::npos);
}

}  // namespace
