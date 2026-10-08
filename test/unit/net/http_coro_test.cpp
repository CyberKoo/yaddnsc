//
// Unit tests for the coroutine HTTP protocol layer, driven by a scripted
// in-memory stream (the net::Stream injection point).
//
// Covers: framing per framing style, 1xx interim handling, the upgrade reject,
// keep-alive carry-over, the lazy chunked read window and the redirect policy.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; these tests use EXPECT_* only.
//

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "infrastructure/coro/coro.h"
#include "infrastructure/net/http/protocol/exchange.h"
#include "infrastructure/net/http/protocol/read_window.h"
#include "infrastructure/net/http/protocol/wire.h"
#include "infrastructure/net/http/redirect.h"
#include "infrastructure/net/http/wire_request.h"
#include "infrastructure/net/stream.h"
#include "infrastructure/net/http/uri.h"

namespace {

using http::ErrorCode;
using http::protocol::ReadWindow;

/// A scripted in-memory stream: serves the canned response in `chunk`-sized
/// reads and records everything written to it.
class ScriptedStream final : public net::Stream {
public:
    explicit ScriptedStream(std::string incoming, const std::size_t chunk = 4096)
        : incoming_(std::move(incoming)), chunk_(chunk) {}

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> ensure_connected() override { co_return {}; }

    [[nodiscard]] coro::Task<std::expected<std::size_t, net::IoError>> read_some(
        const std::span<std::uint8_t> buf) override {
        if (cursor_ >= incoming_.size()) {
            co_return std::size_t{0};  // EOF
        }
        const std::size_t available = incoming_.size() - cursor_;
        const std::size_t count = std::min({buf.size(), chunk_, available});
        std::memcpy(buf.data(), incoming_.data() + cursor_, count);
        cursor_ += count;
        co_return count;
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> read_exact(const std::span<std::uint8_t> buf) override {
        std::size_t filled = 0;
        while (filled < buf.size()) {
            auto read = co_await read_some(buf.subspan(filled));
            if (!read) {
                co_return std::unexpected(read.error());
            }
            if (*read == 0) {
                co_return std::unexpected(net::IoError::CONNECTION_FAILED);
            }
            filled += *read;
        }
        co_return {};
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> send_all(
        const std::span<const std::uint8_t> data) override {
        sent_.append(reinterpret_cast<const char*>(data.data()), data.size());
        co_return {};
    }

    void close() noexcept override {}

    [[nodiscard]] bool connected() const noexcept override { return true; }

    [[nodiscard]] const std::string& sent() const noexcept { return sent_; }

private:
    std::string incoming_;
    std::size_t chunk_;
    std::size_t cursor_{0};
    std::string sent_;
};

[[nodiscard]] http::protocol::WireRequest get_request(std::string target = "/index.html") {
    http::protocol::WireRequest request;
    request.method = http::Method::GET;
    request.target = std::move(target);
    request.headers.emplace("Host", "example.test");
    return request;
}

/// Run one exchange against a scripted stream.
[[nodiscard]] std::expected<http::protocol::RawResponse, http::Error> run_exchange(
    ScriptedStream& stream, const http::protocol::WireRequest& request, std::string& pending,
    const http::Limits& limits = {}) {
    return coro::run([&]() -> coro::Task<std::expected<http::protocol::RawResponse, http::Error>> {
        co_return co_await http::protocol::exchange(stream, request, limits, pending);
    }());
}

// ---------------------------------------------------------------------------
// response framing
// ---------------------------------------------------------------------------

TEST(HttpExchange, content_length_body_DecodesAndReadsPastTheBuffer) {
    ScriptedStream stream{"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->text(), "hello");
    // Bytes past the body belong to the next response, not to this one.
    EXPECT_EQ(pending, "EXTRA");
    EXPECT_TRUE(stream.sent().starts_with("GET /index.html HTTP/1.1\r\n"));
}

TEST(HttpExchange, chunked_body_WithExtensionsAndTrailers_Decodes) {
    ScriptedStream stream{
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5;ext=\"v\"\r\nhello\r\n"
        "6\r\n world\r\n"
        "0\r\nX-Trailer: yes\r\n\r\n"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->text(), "hello world");
    ASSERT_EQ(response->trailers.size(), 1U);
    EXPECT_EQ(response->trailers.begin()->first, "X-Trailer");
    EXPECT_EQ(response->trailers.begin()->second, "yes");
    // HTTP/1.1 without a Connection: close is reusable.
}

TEST(HttpExchange, chunked_manyOneByteReads_DecodesIdentically) {
    // 4000 one-byte chunks, each delivered as its own one-byte read, so every
    // incremental branch of the decoder runs and the read window is exercised
    // with the smallest possible steps.
    std::string wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    std::string expected;
    for (int i = 0; i < 4000; ++i) {
        const char digit = static_cast<char>('a' + (i % 26));
        wire += "1\r\n";
        wire += digit;
        wire += "\r\n";
        expected += digit;
    }
    wire += "0\r\n\r\n";

    ScriptedStream stream{std::move(wire), 1};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->size(), expected.size());
    EXPECT_EQ(response->text(), expected);
}

TEST(HttpExchange, interim_100Continue_IsConsumedBeforeTheFinalResponse) {
    ScriptedStream stream{
        "HTTP/1.1 100 Continue\r\n\r\n"
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->text(), "ok");
}

TEST(HttpExchange, upgrade_101_IsRejected) {
    ScriptedStream stream{"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::UNSUPPORTED_PROTOCOL);
}

TEST(HttpExchange, noBody_204_LeavesTheBodyEmptyAndStaysReusable) {
    ScriptedStream stream{"HTTP/1.1 204 No Content\r\n\r\n"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status, 204);
    EXPECT_TRUE(response->text().empty());
    // 204 has no body framing, and the request did not ask to close, so the
    // connection is reusable under HTTP/1.1.
    EXPECT_TRUE(response->reusable);
}

TEST(HttpExchange, keepAlive_twoResponsesInOneRead_CarryOverThroughPending) {
    ScriptedStream stream{
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\none"
        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\ntwo"};
    std::string pending;

    const auto first = run_exchange(stream, get_request("/first"), pending);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->text(), "one");
    EXPECT_TRUE(pending.starts_with("HTTP/1.1 200 OK"));

    const auto second = run_exchange(stream, get_request("/second"), pending);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->text(), "two");
    EXPECT_TRUE(pending.empty());
    EXPECT_TRUE(second->reusable);
}

TEST(HttpExchange, conflicting_framing_IsRejected) {
    ScriptedStream stream{"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"};
    std::string pending;
    const auto response = run_exchange(stream, get_request(), pending);

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::RESPONSE_PARSE_FAILED);
}

TEST(HttpExchange, headers_overTheLimit_Fail) {
    std::string wire = "HTTP/1.1 200 OK\r\nX-Filler: ";
    wire += std::string(200, 'a');
    wire += "\r\nContent-Length: 0\r\n\r\n";

    ScriptedStream stream{std::move(wire)};
    std::string pending;
    http::Limits limits;
    limits.max_header_bytes = 64;
    const auto response = run_exchange(stream, get_request(), pending, limits);

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::HEADERS_TOO_LARGE);
}

// ---------------------------------------------------------------------------
// the lazy read window
// ---------------------------------------------------------------------------

TEST(ReadWindow, consumingTinyIncrements_KeepsTheLiveBufferBounded) {
    // One byte appended and one byte consumed per step, 100000 times: the live
    // buffer must never grow with the amount consumed, which is what makes the
    // decoder linear instead of quadratic.
    ReadWindow window{"x"};
    std::size_t max_live = 0;
    for (int i = 0; i < 100000; ++i) {
        window.append("y", 1);
        window.consume(1);
        max_live = std::max(max_live, window.capacity_in_use());
    }

    EXPECT_EQ(window.size(), 1U);  // the first byte was never consumed
    EXPECT_GE(window.compacted_bytes(), 100000U - 2 * ReadWindow::COMPACT_THRESHOLD);  // dropped, not copied
    EXPECT_LT(max_live, 2 * ReadWindow::COMPACT_THRESHOLD + 8);
}

TEST(ReadWindow, take_rest_EmptiesTheWindow) {
    ReadWindow window{"abcdef"};
    window.consume(3);
    EXPECT_EQ(window.take_rest(), "def");
    EXPECT_EQ(window.size(), 0U);
    EXPECT_TRUE(window.view().empty());
}

// ---------------------------------------------------------------------------
// redirect policy
// ---------------------------------------------------------------------------

TEST(HttpRedirect, moved_permanently_RewritesToGetAndKeepsTheOrigin) {
    const auto current = get_request("/old");
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;

    const auto evaluation = http::evaluate_redirect(301, {{"Location", "/new?q=1"}}, 0, options, current, uri);

    ASSERT_TRUE(evaluation.plan.has_value());
    EXPECT_FALSE(evaluation.plan->cross_origin);
    EXPECT_EQ(evaluation.plan->next.method, http::Method::GET);
    EXPECT_EQ(evaluation.plan->next.target, "/new?q=1");
    EXPECT_FALSE(evaluation.plan->next.body.has_value());
}

TEST(HttpRedirect, temporary_redirect_307_PreservesMethodAndBody) {
    auto current = get_request("/old");
    current.method = http::Method::POST;
    current.body = "payload";
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;

    const auto evaluation =
        http::evaluate_redirect(307, {{"Location", "https://example.test/new"}}, 0, options, current, uri);

    ASSERT_TRUE(evaluation.plan.has_value());
    ASSERT_TRUE(evaluation.plan->next.body.has_value());
    EXPECT_EQ(*evaluation.plan->next.body, "payload");
    EXPECT_EQ(evaluation.plan->next.method, http::Method::POST);
    ASSERT_EQ(evaluation.plan->next.headers.count("Content-Length"), 1U);
}

TEST(HttpRedirect, cross_origin_DropsCredentials) {
    auto current = get_request("/old");
    current.headers.emplace("Authorization", "Bearer secret");
    current.headers.emplace("Cookie", "session=1");
    current.headers.emplace("X-Keep", "yes");
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;

    const auto evaluation =
        http::evaluate_redirect(302, {{"Location", "https://other.test/new"}}, 0, options, current, uri);

    ASSERT_TRUE(evaluation.plan.has_value());
    EXPECT_TRUE(evaluation.plan->cross_origin);
    EXPECT_EQ(evaluation.plan->next.headers.count("Authorization"), 0U);
    EXPECT_EQ(evaluation.plan->next.headers.count("Cookie"), 0U);
    EXPECT_EQ(evaluation.plan->next.headers.count("X-Keep"), 1U);
}

TEST(HttpRedirect, https_to_http_downgrade_IsNotFollowed) {
    const auto current = get_request("/old");
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;

    const auto evaluation =
        http::evaluate_redirect(302, {{"Location", "http://example.test/new"}}, 0, options, current, uri);

    EXPECT_FALSE(evaluation.plan.has_value());
    EXPECT_FALSE(evaluation.limit_reached);
}

TEST(HttpRedirect, beyondTheLimit_ReportsTheLimit) {
    const auto current = get_request("/old");
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;
    options.max_redirects = 2;

    const auto evaluation = http::evaluate_redirect(302, {{"Location", "/new"}}, 2, options, current, uri);

    EXPECT_FALSE(evaluation.plan.has_value());
    EXPECT_TRUE(evaluation.limit_reached);
}

TEST(HttpRedirect, disabledFollowing_LeavesTheResponseAlone) {
    const auto current = get_request("/old");
    const auto uri = Uri::parse("https://example.test/old").value();
    http::Options options;
    options.follow_redirects = false;

    const auto evaluation = http::evaluate_redirect(302, {{"Location", "/new"}}, 0, options, current, uri);

    EXPECT_FALSE(evaluation.plan.has_value());
    EXPECT_FALSE(evaluation.limit_reached);
}

}  // namespace
