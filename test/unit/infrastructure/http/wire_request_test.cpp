//
// Unit tests for the HTTP request builder (src/infrastructure/http/wire_request.cpp).
//
// Covers: the request-target assembly, the user-header validation rules, the
// wire-request guard (target grammar, framing/routing consistency), the
// client-owned header set (Host / Connection / User-Agent / Content-Length /
// Content-Type), the wire-to-public round trip used to replay a redirect hop,
// query-stripped diagnostic URLs, and the transport-failure reason phrases.
//
// Pure value logic: no stream, no coroutine, no I/O.
//

#include "infrastructure/http/wire_request.h"

#include <expected>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "infrastructure/http/protocol/wire.h"
#include "infrastructure/http/types.h"
#include "infrastructure/uri/uri.h"

namespace {

using http::Error;
using http::ErrorCode;

/// A wire request as (name, value), so a table of header cases stays readable.
using Header = std::pair<std::string, std::string>;

/// The first value of `name`, or "" when the header is absent. The wire request
/// is a multimap, so the client-owned fields are checked by name.
[[nodiscard]] std::string header_value(const http::protocol::WireRequest& wire, const std::string_view name) {
    const auto range = wire.headers.equal_range(std::string(name));
    return range.first == range.second ? std::string{} : range.first->second;
}

/// The Connection directive a request is serialized with, or "" when it states none.
[[nodiscard]] std::string connection_directive(const http::HttpVersion version, const bool keep_alive) {
    const http::Request request;
    http::Options options;
    options.version = version;
    options.keep_alive = keep_alive;
    return header_value(http::build_wire_request(request, "https", "example.test", 443, options), "Connection");
}

[[nodiscard]] std::string reason_for(const ErrorCode code) {
    return http::failure_reason("API server", Error{code, {}});
}

// ---------------------------------------------------------------------------
// request target
// ---------------------------------------------------------------------------

TEST(HttpWireRequestTarget, isThePathAndQueryOfTheUri) {
    const auto query = Uri::parse("https://example.test/dns-query?name=example.test&type=A");
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(http::make_target(*query), "/dns-query?name=example.test&type=A");

    // An explicitly empty query keeps its delimiter, so the peer still sees that
    // a query was present and an empty string was sent as its value.
    const auto empty_query = Uri::parse("https://example.test/dns-query?");
    ASSERT_TRUE(empty_query.has_value());
    EXPECT_EQ(http::make_target(*empty_query), "/dns-query?");
}

TEST(HttpWireRequestTarget, emptyPath_BecomesTheRootPath) {
    // A URI that carries no path at all (a relative reference, or empty input)
    // must still produce an origin-form target rather than an empty request line.
    const auto empty = Uri::parse("");
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(http::make_target(*empty), "/");

    // A scheme URI normalises its empty path to "/" before it reaches the builder.
    const auto origin = Uri::parse("https://example.test");
    ASSERT_TRUE(origin.has_value());
    EXPECT_EQ(http::make_target(*origin), "/");

    // An origin with only a query keeps the root path in front of the delimiter.
    const auto root_with_query = Uri::parse("https://example.test?name=example.test");
    ASSERT_TRUE(root_with_query.has_value());
    EXPECT_EQ(http::make_target(*root_with_query), "/?name=example.test");
}

// ---------------------------------------------------------------------------
// request validation
// ---------------------------------------------------------------------------

TEST(HttpWireRequestValidate, fieldNamesMustBeTokens) {
    http::Request request;
    request.headers.emplace("Invalid Header", "value");

    const auto result = http::validate_request(request);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::INVALID_REQUEST);
    EXPECT_EQ(result.error().message, "invalid HTTP request header");
}

TEST(HttpWireRequestValidate, fieldValuesMustNotCarryCrlf) {
    http::Request request;
    request.headers.emplace("X-Trace", "value\r\nInjected: yes");

    const auto result = http::validate_request(request);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::INVALID_REQUEST);
    EXPECT_EQ(result.error().message, "invalid HTTP request header");
}

TEST(HttpWireRequestValidate, contentTypeMustNotCarryCrlf) {
    http::Request request;
    request.content_type = "text/plain\r\nInjected: yes";

    const auto result = http::validate_request(request);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::INVALID_REQUEST);
    EXPECT_EQ(result.error().message, "invalid HTTP Content-Type");
}

TEST(HttpWireRequestValidate, protocolUpgradeIsRejected) {
    http::Request request;
    request.headers.emplace("Upgrade", "h2c");

    const auto result = http::validate_request(request);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::UNSUPPORTED_PROTOCOL);
    EXPECT_EQ(result.error().message, "HTTP protocol upgrade is not supported");
}

TEST(HttpWireRequestValidate, transferCodingAndTrailersAreRejected) {
    // Both request headers would let the caller re-frame the body the client
    // already framed, so neither is accepted from the caller.
    for (const auto& [name, value] : {std::pair{"Transfer-Encoding", "chunked"}, std::pair{"Trailer", "X-Checksum"}}) {
        SCOPED_TRACE(name);
        http::Request request;
        request.headers.emplace(name, value);

        const auto result = http::validate_request(request);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::INVALID_REQUEST);
        EXPECT_EQ(result.error().message, "request transfer coding and trailers are not supported");
    }
}

TEST(HttpWireRequestValidate, aWellFormedRequestPasses) {
    http::Request request;
    request.headers.emplace("X-Trace", "abc");
    request.set_body("payload");
    request.content_type = "application/dns-message";

    EXPECT_TRUE(http::validate_request(request).has_value());
}

// ---------------------------------------------------------------------------
// wire request guard: the request target
// ---------------------------------------------------------------------------

[[nodiscard]] http::protocol::WireRequest wire_to(std::string target = "/dns-query") {
    http::protocol::WireRequest wire;
    wire.method = http::Method::GET;
    wire.target = std::move(target);
    wire.headers.emplace("Host", "example.test");
    return wire;
}

[[nodiscard]] http::protocol::WireRequest wire_with(const Header& header) {
    auto wire = wire_to();
    wire.headers.emplace(header.first, header.second);
    return wire;
}

TEST(HttpWireRequestGuard, asteriskFormIsAccepted) {
    EXPECT_TRUE(http::validate_wire_request(wire_to("*")).has_value());
}

TEST(HttpWireRequestGuard, originFormIsRequired) {
    for (const auto& target : {"", "dns-query", "/dns query"}) {
        SCOPED_TRACE(target);
        const auto result = http::validate_wire_request(wire_to(target));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, ErrorCode::INVALID_REQUEST);
        EXPECT_EQ(result.error().message, "invalid HTTP request target");
    }
}

// ---------------------------------------------------------------------------
// wire request guard: header fields
// ---------------------------------------------------------------------------

/// Expect the wire guard to reject `wire` with `code` and `message`.
void expect_wire_rejected(const http::protocol::WireRequest& wire, const ErrorCode code,
                          const std::string_view message) {
    const auto result = http::validate_wire_request(wire);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, code);
    EXPECT_EQ(result.error().message, message);
}

TEST(HttpWireRequestGuard, namesMustBeTokensAndValuesPrintable) {
    for (const auto& header : {Header{"Invalid Header", "value"}, Header{"X-Trace", "value\r\nInjected: yes"}}) {
        SCOPED_TRACE(header.first);
        expect_wire_rejected(wire_with(header), ErrorCode::INVALID_REQUEST, "invalid HTTP request header");
    }
}

TEST(HttpWireRequestGuard, connectionMustBeATokenList) {
    expect_wire_rejected(wire_with({"Connection", "clo se"}), ErrorCode::INVALID_REQUEST,
                         "invalid HTTP request header");
}

TEST(HttpWireRequestGuard, upgradeIsRejectedInEitherForm) {
    // A direct Upgrade field and the Connection-token spelling must both fail:
    // this client speaks HTTP/1.x framing only.
    for (const auto& header : {Header{"Upgrade", "websocket"}, Header{"Connection", "upgrade"}}) {
        SCOPED_TRACE(header.first);
        expect_wire_rejected(wire_with(header), ErrorCode::UNSUPPORTED_PROTOCOL,
                             "HTTP protocol upgrade is not supported");
    }
}

TEST(HttpWireRequestGuard, transferCodingAndTrailersAreRejected) {
    // Either field would let the caller re-frame a body the client already
    // framed, which is a request-smuggling vector.
    for (const auto& header : {Header{"Transfer-Encoding", "chunked"}, Header{"Trailer", "X-Checksum"}}) {
        SCOPED_TRACE(header.first);
        expect_wire_rejected(wire_with(header), ErrorCode::INVALID_REQUEST,
                             "request transfer coding and trailers are not supported");
    }
}

TEST(HttpWireRequestGuard, contentLengthMustBeNumeric) {
    for (const auto& value : {"abc", "", "1 2"}) {
        SCOPED_TRACE(value);
        expect_wire_rejected(wire_with({"Content-Length", value}), ErrorCode::INVALID_REQUEST,
                             "invalid HTTP Content-Length");
    }
}

TEST(HttpWireRequestGuard, duplicateRoutingOrFramingHeadersConflict) {
    // Two Host fields, two Connection fields and two Content-Length fields each
    // leave the peer's routing and framing ambiguous.
    expect_wire_rejected(wire_with({"Host", "other.test"}), ErrorCode::INVALID_REQUEST,
                         "conflicting HTTP framing or routing headers");

    auto connections = wire_to();
    connections.headers.emplace("Connection", "close");
    connections.headers.emplace("Connection", "keep-alive");
    expect_wire_rejected(connections, ErrorCode::INVALID_REQUEST, "conflicting HTTP framing or routing headers");

    auto lengths = wire_to();
    lengths.headers.emplace("Content-Length", "0");
    lengths.headers.emplace("Content-Length", "0");
    expect_wire_rejected(lengths, ErrorCode::INVALID_REQUEST, "conflicting HTTP framing or routing headers");
}

TEST(HttpWireRequestGuard, contentLengthMustMatchTheBodyActuallySent) {
    expect_wire_rejected(wire_with({"Content-Length", "5"}), ErrorCode::INVALID_REQUEST,
                         "conflicting HTTP framing or routing headers");

    auto agreed = wire_with({"Content-Length", "5"});
    agreed.body = "hello";
    EXPECT_TRUE(http::validate_wire_request(agreed).has_value());
}

TEST(HttpWireRequestGuard, aBuiltRequestPasses) {
    // What the builder emits is always valid, so no client re-derives the rules.
    http::Request request;
    request.headers.emplace("X-Trace", "abc");
    request.set_body("payload");
    request.content_type = "application/dns-message";
    http::Options options;

    auto wire = http::build_wire_request(request, "https", "example.test", 443, options);
    wire.target = "/dns-query?token=secret";

    EXPECT_TRUE(http::validate_wire_request(wire).has_value());
}

// ---------------------------------------------------------------------------
// wire request construction
// ---------------------------------------------------------------------------

TEST(HttpWireRequestBuild, hostHeaderFollowsRfc7230) {
    // The well-known port is implied by the scheme and therefore omitted.
    EXPECT_EQ(http::make_host_header("https", "example.test", 443), "example.test");
    EXPECT_EQ(http::make_host_header("http", "example.test", 80), "example.test");
    // Any other port is stated, and an IP-literal is bracketed.
    EXPECT_EQ(http::make_host_header("http", "example.test", 8080), "example.test:8080");
    EXPECT_EQ(http::make_host_header("https", "2001:db8::1", 443), "[2001:db8::1]");
    EXPECT_EQ(http::make_host_header("https", "2001:db8::1", 8443), "[2001:db8::1]:8443");
}

TEST(HttpWireRequestBuild, connectionDirectiveFollowsTheVersionAndKeepAlivePolicy) {
    // HTTP/1.0 has no persistent-connection default, so the client always states
    // its intent; HTTP/1.1 only states an opt-out.
    EXPECT_EQ(connection_directive(http::HttpVersion::V1_0, true), "keep-alive");
    EXPECT_EQ(connection_directive(http::HttpVersion::V1_0, false), "close");
    EXPECT_EQ(connection_directive(http::HttpVersion::V1_1, true), "");
    EXPECT_EQ(connection_directive(http::HttpVersion::V1_1, false), "close");
}

TEST(HttpWireRequestBuild, closesConnectionAnswersTheSessionReuseQuestion) {
    // The session asks this instead of re-reading the emitted Connection field.
    http::Options persistent;
    EXPECT_FALSE(http::closes_connection(persistent));
    persistent.keep_alive = false;
    EXPECT_TRUE(http::closes_connection(persistent));
}

TEST(HttpWireRequestBuild, clientOwnedHeadersReplaceTheCallersCopies) {
    http::Request request;
    request.headers.emplace("Host", "user.test");
    request.headers.emplace("User-Agent", "user-agent/0");
    request.headers.emplace("Connection", "keep-alive");
    request.headers.emplace("X-Trace", "abc");
    request.set_body("payload");
    request.content_type = "application/dns-message";

    http::Options options;
    options.user_agent = "yaddnsc/1";

    const auto wire = http::build_wire_request(request, "https", "example.test", 443, options);

    // Exactly one Host / User-Agent survives, and it is the client's copy.
    EXPECT_EQ(wire.headers.count("Host"), 1U);
    EXPECT_EQ(wire.headers.count("User-Agent"), 1U);
    EXPECT_EQ(header_value(wire, "Host"), "example.test");
    EXPECT_EQ(header_value(wire, "User-Agent"), "yaddnsc/1");
    EXPECT_EQ(header_value(wire, "Content-Length"), "7");
    EXPECT_EQ(header_value(wire, "Content-Type"), "application/dns-message");
    // The caller's Connection directive is dropped; the builder's own policy
    // decides, and here that policy emitted none.
    EXPECT_EQ(wire.headers.count("Connection"), 0U);
    // Application headers are untouched.
    EXPECT_EQ(header_value(wire, "X-Trace"), "abc");
}

TEST(HttpWireRequestBuild, bodylessRequestsCarryNoFramingHeaders) {
    http::Request request;
    request.content_type = "application/dns-message";

    const auto wire = http::build_wire_request(request, "https", "example.test", 443, http::Options{});

    EXPECT_FALSE(wire.body.has_value());
    EXPECT_EQ(wire.headers.count("Content-Length"), 0U);
    // Content-Type describes a body, so it only rides along with one.
    EXPECT_EQ(wire.headers.count("Content-Type"), 0U);
}

TEST(HttpWireRequestBuild, absoluteUrlBracketsIpv6) {
    EXPECT_EQ(http::make_absolute_url("https", "2001:db8::1", 443, "/query?x=1"),
              "https://[2001:db8::1]:443/query?x=1");
}

// ---------------------------------------------------------------------------
// transport failure phrasing
// ---------------------------------------------------------------------------

TEST(HttpWireRequestFailure, reasonPhrasesNameTheServerByRole) {
    EXPECT_EQ(reason_for(ErrorCode::RESOLVE_FAILED), "cannot resolve the API server");
    EXPECT_EQ(reason_for(ErrorCode::QUEUE_TIMEOUT), "the local session for the API server remained busy");
    EXPECT_EQ(reason_for(ErrorCode::CONNECT_FAILED), "cannot connect to the API server");
    EXPECT_EQ(reason_for(ErrorCode::TLS_HANDSHAKE_FAILED), "TLS handshake with the API server failed");
    EXPECT_EQ(reason_for(ErrorCode::CONNECTION_LOST), "the connection to the API server was lost");
    EXPECT_EQ(reason_for(ErrorCode::RESPONSE_PARSE_FAILED), "the API server returned an invalid response");
    EXPECT_EQ(reason_for(ErrorCode::HEADERS_TOO_LARGE), "the API server returned an oversized response");
    EXPECT_EQ(reason_for(ErrorCode::BODY_TOO_LARGE), "the API server returned an oversized response");
    EXPECT_EQ(reason_for(ErrorCode::REDIRECT_LIMIT_EXCEEDED), "the API server redirected too many times");
    // A bad URL or a rejected request is a local mistake, so it names neither the
    // peer nor the role it was aimed at.
    EXPECT_EQ(reason_for(ErrorCode::INVALID_URL), "the configured URL is invalid");
    EXPECT_EQ(reason_for(ErrorCode::INVALID_REQUEST), "the request is invalid or unsupported");
    EXPECT_EQ(reason_for(ErrorCode::UNSUPPORTED_PROTOCOL), "the request is invalid or unsupported");
}

TEST(HttpWireRequestFailure, reasonNamesTheRoleNotTheHost) {
    // The role is the caller's label for the peer; credentials in a query string
    // never reach a log line this way.
    EXPECT_EQ(http::failure_reason("IP source server", Error{ErrorCode::CONNECT_FAILED, {}}),
              "cannot connect to the IP source server");
    EXPECT_EQ(http::failure_reason("IP source server", Error{ErrorCode::RESOLVE_FAILED, {}}),
              "cannot resolve the IP source server");
}

// ---------------------------------------------------------------------------
// diagnostic URLs
// ---------------------------------------------------------------------------

TEST(HttpRedaction, queryStringIsRemovedFromAUrlOrTarget) {
    // A DoH key or an access token travels in the query, so no diagnostic may
    // quote it.
    EXPECT_EQ(http::redacted_url("https://example.test/dns-query?dns=AA&id=secret"), "https://example.test/dns-query");
    EXPECT_EQ(http::redacted_url("/v1/update?token=secret"), "/v1/update");
    EXPECT_EQ(http::redacted_url("https://example.test/dns-query"), "https://example.test/dns-query");
    EXPECT_EQ(http::redacted_url(""), "");
}

}  // namespace
