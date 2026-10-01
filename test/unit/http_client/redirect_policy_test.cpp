//
// Unit tests for the redirect policy
// (src/infrastructure/network/http/redirect.cpp).
//
// evaluate_redirect() is pure decision logic, so every RFC 3986 §5.2
// reference form and every rejection rule is driven directly:
//   - reference resolution: absolute, scheme-relative, root-relative,
//     path-relative, query-only, fragment-only
//   - RFC 1034 §5.1.4 dot-segment removal, including trailing slashes
//   - method/body rewriting: 307/308 preserve, 301/302/303 become GET
//   - origin comparison, auth-header stripping on cross-origin hops
//   - rejection: non-3xx, no Location, following disabled, limit reached,
//     https -> http downgrade, unusable scheme
// =============================================================================

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "infrastructure/network/http/redirect.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/network/uri.h"

using net::http::HttpVersion;
using net::http::Method;
using net::http::Options;
using net::http::RedirectEval;
using net::http::evaluate_redirect;
using net::http::protocol::WireRequest;

namespace {

Uri must_parse(std::string_view text) {
    auto parsed = Uri::parse(text);
    if (!parsed.has_value()) {
        ADD_FAILURE() << "failed to parse URI: " << text;
    }
    return *parsed;
}

std::multimap<std::string, std::string> location_headers(std::string_view value) {
    return {{"Location", std::string(value)}};
}

/// A current request as it would look on the wire.
WireRequest current_request(std::string target = "/start") {
    WireRequest req{
        .method = Method::POST,
        .version = HttpVersion::V1_1,
        .target = std::move(target),
        .headers = {{"Host", "origin.example"}, {"Content-Type", "application/dns-message"},
                    {"Content-Length", "9"}},
        .body = std::string("some-body"),
    };
    return req;
}

Options default_options() {
    Options opts;
    opts.follow_redirects = true;
    opts.max_redirects = 10;
    return opts;
}

std::string header_value(const WireRequest& wire, std::string_view name) {
    for (const auto& [key, value] : wire.headers) {
        if (key.size() == name.size() &&
            std::equal(key.begin(), key.end(), name.begin(),
                       [](char a, char b) { return (a | 0x20) == (b | 0x20); })) {
            return value;
        }
    }
    return {};
}

RedirectEval evaluate(int status, std::string_view location, std::string_view current_uri_text, int count = 0,
                      Options opts = default_options()) {
    return evaluate_redirect(status, location_headers(location), count, opts, current_request(),
                             must_parse(current_uri_text));
}

// ===========================================================================
// Rejection rules
// ===========================================================================

TEST(RedirectPolicy, NonRedirectStatus_IsNotFollowed) {
    const auto result = evaluate(200, "/elsewhere", "https://origin.example/start");
    EXPECT_FALSE(result.plan.has_value());
    EXPECT_FALSE(result.limit_reached);
}

TEST(RedirectPolicy, MissingLocationHeader_IsNotFollowed) {
    const std::multimap<std::string, std::string> headers{{"Content-Type", "text/plain"}};
    const auto result = evaluate_redirect(302, headers, 0, default_options(), current_request(),
                                          must_parse("https://origin.example/start"));
    EXPECT_FALSE(result.plan.has_value());
}

TEST(RedirectPolicy, BlankLocationValue_IsNotFollowed) {
    const auto result = evaluate(302, "   ", "https://origin.example/start");
    EXPECT_FALSE(result.plan.has_value());
}

TEST(RedirectPolicy, FollowRedirectsDisabled_IsNotFollowed) {
    auto opts = default_options();
    opts.follow_redirects = false;
    const auto result = evaluate(302, "/elsewhere", "https://origin.example/start", 0, opts);
    EXPECT_FALSE(result.plan.has_value());
    EXPECT_FALSE(result.limit_reached);
}

TEST(RedirectPolicy, LimitReached_ReportsFailureRatherThanPlan) {
    auto opts = default_options();
    opts.max_redirects = 3;
    const auto result = evaluate(302, "/elsewhere", "https://origin.example/start", 3, opts);
    EXPECT_FALSE(result.plan.has_value());
    EXPECT_TRUE(result.limit_reached);
}

TEST(RedirectPolicy, BelowLimit_ProducesAPlan) {
    auto opts = default_options();
    opts.max_redirects = 3;
    const auto result = evaluate(302, "/elsewhere", "https://origin.example/start", 2, opts);
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_FALSE(result.limit_reached);
}

TEST(RedirectPolicy, HttpsToHttpDowngrade_IsNotFollowed) {
    const auto result = evaluate(301, "http://insecure.example/next", "https://origin.example/start");
    EXPECT_FALSE(result.plan.has_value());
}

TEST(RedirectPolicy, NonHttpScheme_IsNotFollowed) {
    // ftp:// is syntactically absolute but is not a usable HTTP target.
    const auto result = evaluate(301, "ftp://files.example/x", "https://origin.example/start");
    EXPECT_FALSE(result.plan.has_value());
}

TEST(RedirectPolicy, AbsoluteUrlWithoutHost_IsNotFollowed) {
    const auto result = evaluate(301, "http:///no-host", "https://origin.example/start");
    EXPECT_FALSE(result.plan.has_value());
}

// ===========================================================================
// Reference resolution (RFC 3986 §5.2)
// ===========================================================================

TEST(RedirectPolicy, AbsoluteUrl_ReplacesOriginEntirely) {
    // Must stay on https: an https -> http hop is refused (see the downgrade case).
    const auto result = evaluate(301, "https://other.example:8080/next?x=1", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->scheme, "https");
    EXPECT_EQ(result.plan->host, "other.example");
    EXPECT_EQ(result.plan->port, 8080);
    EXPECT_EQ(result.plan->next.target, "/next?x=1");
    EXPECT_TRUE(result.plan->cross_origin);
}

TEST(RedirectPolicy, AbsoluteUrlWithDefaultPort_FillsTheDefault) {
    const auto result = evaluate(301, "https://other.example/next", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->port, 443);
    EXPECT_EQ(header_value(result.plan->next, "Host"), "other.example");
}

TEST(RedirectPolicy, SchemeRelativeUrl_InheritsTheCurrentScheme) {
    const auto result = evaluate(301, "//other.example/next", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->scheme, "https");
    EXPECT_EQ(result.plan->host, "other.example");
    EXPECT_EQ(result.plan->port, 443);
}

TEST(RedirectPolicy, RootRelativeUrl_KeepsTheOrigin) {
    const auto result = evaluate(301, "/fresh/path?a=b", "https://origin.example/old/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->host, "origin.example");
    EXPECT_EQ(result.plan->next.target, "/fresh/path?a=b");
    EXPECT_FALSE(result.plan->cross_origin);
}

TEST(RedirectPolicy, PathRelativeUrl_ResolvesAgainstTheBaseDirectory) {
    const auto result = evaluate(301, "sibling", "https://origin.example/dir/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.target, "/dir/sibling");
}

TEST(RedirectPolicy, DotDotSegment_WalksUpThePath) {
    const auto result = evaluate(301, "../up", "https://origin.example/a/b/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.target, "/a/up");
}

TEST(RedirectPolicy, QueryOnlyUrl_ReusesTheCurrentPath) {
    const auto result = evaluate(301, "?only=query", "https://origin.example/base/path");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.target, "/base/path?only=query");
}

TEST(RedirectPolicy, FragmentOnlyUrl_ReusesPathAndQuery) {
    // Fragments are never sent on the wire; the target stays path + query.
    const auto result = evaluate(301, "#section", "https://origin.example/base/path?keep=1");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.target, "/base/path?keep=1");
}

TEST(RedirectPolicy, FragmentOnlyUrl_WithNoQuery_ReusesThePath) {
    const auto result = evaluate(301, "#section", "https://origin.example/base/path");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.target, "/base/path");
}

TEST(RedirectPolicy, LocationWithFragment_StripsTheFragment) {
    const auto result = evaluate(301, "/next?x=1#frag", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.target, "/next?x=1");
}

TEST(RedirectPolicy, LocationWithSurroundingWhitespace_IsTrimmed) {
    const auto result = evaluate(301, "  /next  ", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.target, "/next");
}

TEST(RedirectPolicy, DotSegmentsInAbsoluteUrl_AreRemoved) {
    const auto result = evaluate(301, "https://origin.example/a/b/../c/./d", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.target, "/a/c/d");
}

TEST(RedirectPolicy, TrailingSlashInPath_IsPreserved) {
    const auto result = evaluate(301, "/dir/", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.target, "/dir/");
}

TEST(RedirectPolicy, DotDotAtTheRoot_IsClamped) {
    // ".." with nothing left to pop must not escape the root.
    const auto result = evaluate(301, "/../escape", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.target, "/escape");
}

TEST(RedirectPolicy, SameOriginDifferentPort_IsCrossOrigin) {
    const auto result = evaluate(301, "https://origin.example:8443/next", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_TRUE(result.plan->cross_origin);
    EXPECT_EQ(result.plan->port, 8443);
}

TEST(RedirectPolicy, CurrentUriWithoutExplicitPort_ComparesAgainstTheDefault) {
    // "https://origin.example" and "https://origin.example:443" are the same
    // origin, so this must not be flagged cross-origin.
    const auto result = evaluate(301, "https://origin.example:443/next", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_FALSE(result.plan->cross_origin);
}

// ===========================================================================
// Method and body rewriting
// ===========================================================================

TEST(RedirectPolicy, Status307_PreservesMethodAndBody) {
    const auto result = evaluate(307, "/next", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.method, Method::POST);
    ASSERT_TRUE(result.plan->next.body.has_value());
    EXPECT_EQ(*result.plan->next.body, "some-body");
    EXPECT_EQ(header_value(result.plan->next, "Content-Length"), "9");
    EXPECT_EQ(header_value(result.plan->next, "Content-Type"), "application/dns-message");
}

TEST(RedirectPolicy, Status308_PreservesMethodAndBody) {
    const auto result = evaluate(308, "/next", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.method, Method::POST);
    ASSERT_TRUE(result.plan->next.body.has_value());
}

TEST(RedirectPolicy, Status301_RewritesToGetAndDropsTheBody) {
    const auto result = evaluate(301, "/next", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.method, Method::GET);
    EXPECT_FALSE(result.plan->next.body.has_value());
    EXPECT_EQ(result.plan->next.headers.count("Content-Length"), 0u);
    EXPECT_EQ(result.plan->next.headers.count("Content-Type"), 0u);
}

TEST(RedirectPolicy, Status302_RewritesToGetAndDropsTheBody) {
    const auto result = evaluate(302, "/next", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.method, Method::GET);
    EXPECT_FALSE(result.plan->next.body.has_value());
}

TEST(RedirectPolicy, Status303_RewritesToGetAndDropsTheBody) {
    const auto result = evaluate(303, "/next", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.method, Method::GET);
    EXPECT_FALSE(result.plan->next.body.has_value());
}

TEST(RedirectPolicy, HostHeaderIsRebuiltForTheTarget) {
    const auto result = evaluate(301, "https://other.example/next", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(header_value(result.plan->next, "Host"), "other.example");
    EXPECT_EQ(result.plan->next.headers.count("Host"), 1u);
}

TEST(RedirectPolicy, HostHeaderForNonDefaultPort_IncludesThePort) {
    const auto result = evaluate(301, "https://other.example:8443/next", "https://origin.example/start");
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(header_value(result.plan->next, "Host"), "other.example:8443");
}

TEST(RedirectPolicy, BodyWithoutContentType_OmitsTheContentTypeHeader) {
    WireRequest req{
        .method = Method::POST,
        .version = HttpVersion::V1_1,
        .target = "/start",
        .headers = {{"Host", "origin.example"}},
        .body = std::string("payload"),
    };
    const auto result = evaluate_redirect(307, location_headers("/next"), 0, default_options(), req,
                                          must_parse("https://origin.example/start"));
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(header_value(result.plan->next, "Content-Length"), "7");
    EXPECT_EQ(result.plan->next.headers.count("Content-Type"), 0u);
}

// ===========================================================================
// Auth headers across origins
// ===========================================================================

WireRequest request_with_auth() {
    WireRequest req = current_request();
    req.headers.emplace("Authorization", "Bearer secret-token");
    req.headers.emplace("Cookie", "session=abc");
    req.headers.emplace("Proxy-Authorization", "Basic xyz");
    req.headers.emplace("X-Trace", "keep-me");
    return req;
}

TEST(RedirectPolicy, CrossOriginHop_StripsAuthorizationAndCookies) {
    const auto result = evaluate_redirect(307, location_headers("https://other.example/next"), 0, default_options(),
                                          request_with_auth(), must_parse("https://origin.example/start"));
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_TRUE(result.plan->cross_origin);
    EXPECT_EQ(result.plan->next.headers.count("Authorization"), 0u);
    EXPECT_EQ(result.plan->next.headers.count("Cookie"), 0u);
    EXPECT_EQ(result.plan->next.headers.count("Proxy-Authorization"), 0u);
    // Non-credential headers survive the hop.
    EXPECT_EQ(header_value(result.plan->next, "X-Trace"), "keep-me");
}

TEST(RedirectPolicy, SameOriginHop_KeepsAuthorizationAndCookies) {
    const auto result = evaluate_redirect(307, location_headers("/next"), 0, default_options(), request_with_auth(),
                                          must_parse("https://origin.example/start"));
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_FALSE(result.plan->cross_origin);
    EXPECT_EQ(header_value(result.plan->next, "Authorization"), "Bearer secret-token");
    EXPECT_EQ(header_value(result.plan->next, "Cookie"), "session=abc");
    EXPECT_EQ(header_value(result.plan->next, "Proxy-Authorization"), "Basic xyz");
}

TEST(RedirectPolicy, CrossOriginHop_OnGetRedirect_AlsoStripsCredentials) {
    // 301 rewrites to GET and drops the body, but credentials must still go.
    const auto result = evaluate_redirect(301, location_headers("https://other.example/next"), 0, default_options(),
                                          request_with_auth(), must_parse("https://origin.example/start"));
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.headers.count("Authorization"), 0u);
    EXPECT_EQ(result.plan->next.headers.count("Cookie"), 0u);
}

TEST(RedirectPolicy, LocationHeaderIsMatchedCaseInsensitively) {
    const std::multimap<std::string, std::string> headers{{"location", "/next"}};
    const auto result = evaluate_redirect(301, headers, 0, default_options(), current_request(),
                                          must_parse("https://origin.example/start"));
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->next.target, "/next");
}

}  // namespace
