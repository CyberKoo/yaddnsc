//
// Unit tests for the transient HTTP client and the transport helpers it sits on
// (src/infrastructure/http/client.cpp and transport.cpp).
//
// Covers: the redirect loop of Client (a hop opens a new connection and
// rebuilds the request), the lost-connection recovery a transient hop inherits
// from the session pipeline, the URL validation guards, and the per-candidate
// connect policy of connect_stream — the unbounded attempt when no connect
// budget is configured, a failure inside the budget, and the walk over several
// candidate addresses.
//
// Everything runs on a scripted in-memory stream (the net::Stream injection
// point) or on a ManualClock; there is no socket, no DNS and no filesystem I/O.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; the coroutines here use EXPECT_* only.
//

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "coro/coro.h"
#include "domain/network/inet_address.h"
#include "infrastructure/http/client.h"
#include "infrastructure/http/error.h"
#include "infrastructure/http/transport.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/stream.h"

namespace {

using namespace std::chrono_literals;

using http::Error;
using http::ErrorCode;
using http::Options;
using http::Request;

const Request NO_REQUEST;

/// What one scripted connection does.
struct Behaviour {
    std::string incoming{};                     ///< bytes the peer serves; empty means an immediate EOF
    std::vector<bool> connect_results{};        ///< ensure_connected() results, consumed in order
    bool connect_afterwards{true};              ///< once `connect_results` is exhausted
    std::chrono::milliseconds connect_stall{};  ///< park inside ensure_connected()
    std::chrono::milliseconds read_stall{};     ///< park once before the first read

    [[nodiscard]] static Behaviour response(std::string wire) { return Behaviour{.incoming = std::move(wire)}; }
};

/// Everything one scripted connection saw.
struct ConnectionRecord {
    std::string sent;
};

/// A scripted in-memory stream: serves `behaviour` and records what was written.
class ScriptedStream : public net::Stream {
public:
    ScriptedStream(Behaviour behaviour, std::shared_ptr<ConnectionRecord> record)
        : behaviour_(std::move(behaviour)), record_(std::move(record)) {}

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> ensure_connected() override {
        if (behaviour_.connect_stall.count() > 0) {
            co_await coro::sleep_for(behaviour_.connect_stall);
        }
        const std::size_t index = connect_calls_++;
        const bool ok = index < behaviour_.connect_results.size() ? behaviour_.connect_results[index]
                                                                  : behaviour_.connect_afterwards;
        co_return ok ? std::expected<void, net::IoError>{} : std::unexpected(net::IoError::CONNECTION_FAILED);
    }

    [[nodiscard]] coro::Task<std::expected<std::size_t, net::IoError>> read_some(
        const std::span<std::uint8_t> buf) override {
        if (behaviour_.read_stall.count() > 0 && !stalled_) {
            stalled_ = true;
            co_await coro::sleep_for(behaviour_.read_stall);
        }
        if (cursor_ >= behaviour_.incoming.size()) {
            co_return std::size_t{0};  // EOF
        }
        const std::size_t available = behaviour_.incoming.size() - cursor_;
        const std::size_t count = std::min({buf.size(), available});
        std::memcpy(buf.data(), behaviour_.incoming.data() + cursor_, count);
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
        record_->sent.append(reinterpret_cast<const char*>(data.data()), data.size());
        co_return {};
    }

    void close() noexcept override {}

    [[nodiscard]] bool connected() const noexcept override { return true; }

private:
    Behaviour behaviour_;
    std::shared_ptr<ConnectionRecord> record_;
    std::size_t connect_calls_{0};
    std::size_t cursor_{0};
    bool stalled_{false};
};

/// Hands out one scripted connection per call, so a multi-hop exchange gets a
/// fresh peer for every connection it opens.
class ScriptedFactory final : public net::StreamFactory {
public:
    explicit ScriptedFactory(std::deque<Behaviour> behaviours = {}) : behaviours_(std::move(behaviours)) {}

    [[nodiscard]] std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&, const net::TlsOptions&,
                                                          std::shared_ptr<const net::TlsContext>) override {
        ++tls_calls;
        return open();
    }

    [[nodiscard]] std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&) override {
        ++tcp_calls;
        return open();
    }

    /// Everything each connection sent, in the order the connections were opened.
    [[nodiscard]] const std::vector<std::shared_ptr<ConnectionRecord>>& connections() const noexcept {
        return records_;
    }

    [[nodiscard]] const std::string& sent_on(const std::size_t index) const { return records_.at(index)->sent; }

    int tcp_calls{0};
    int tls_calls{0};

private:
    [[nodiscard]] std::unique_ptr<net::Stream> open() {
        Behaviour behaviour;
        if (!behaviours_.empty()) {
            behaviour = std::move(behaviours_.front());
            behaviours_.pop_front();
        }
        auto record = std::make_shared<ConnectionRecord>();
        records_.push_back(record);
        return std::make_unique<ScriptedStream>(std::move(behaviour), std::move(record));
    }

    std::deque<Behaviour> behaviours_;
    std::vector<std::shared_ptr<ConnectionRecord>> records_;
};

[[nodiscard]] std::shared_ptr<ScriptedFactory> factory_for(std::deque<Behaviour> behaviours) {
    return std::make_shared<ScriptedFactory>(std::move(behaviours));
}

[[nodiscard]] Options options_for(const std::shared_ptr<net::StreamFactory>& factory) {
    Options options;
    options.factory = factory;
    options.connect_timeout = 0ms;  // no per-address budget unless a case asks for one
    return options;
}

/// Run one transient exchange on the default (real) clock.
[[nodiscard]] std::expected<http::Response, Error> exchange(const Options& options, const std::string& url,
                                                            const Request& request = NO_REQUEST) {
    http::Client client{options};
    return coro::run(client.exchange(url, request));
}

[[nodiscard]] domain::InetAddress address_of(const std::string_view literal) {
    return *domain::InetAddress::parse(literal);
}

// ---------------------------------------------------------------------------
// Client: URL validation
// ---------------------------------------------------------------------------

TEST(HttpClientUrl, unparsableUrl_IsRejectedBeforeAnyConnection) {
    auto factory = factory_for({});
    const Options options = options_for(factory);

    // An unclosed IPv6 bracket is rejected by the URI parser itself.
    const auto response = exchange(options, "http://[::1/");

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::INVALID_URL);
    EXPECT_EQ(factory->tcp_calls, 0);
}

TEST(HttpClientUrl, nonHttpScheme_IsRejected) {
    auto factory = factory_for({});
    const Options options = options_for(factory);

    const auto response = exchange(options, "ftp://203.0.113.10/data");

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::INVALID_URL);
    EXPECT_EQ(response.error().message, R"(invalid URL: "ftp://203.0.113.10/data")");
    EXPECT_EQ(factory->tcp_calls, 0);
}

TEST(HttpClientUrl, hostnameWithoutResolver_FailsWithResolveFailed) {
    auto factory = factory_for({});
    const Options options = options_for(factory);

    const auto response = exchange(options, "http://example.test/dns-query");

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::RESOLVE_FAILED);
    EXPECT_EQ(factory->tcp_calls, 0);
}

TEST(HttpClientUrl, connectFailure_IsReported) {
    auto factory = factory_for({Behaviour{.connect_results = {false}}});
    const Options options = options_for(factory);

    const auto response = exchange(options, "http://203.0.113.10/dns-query");

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::CONNECT_FAILED);
    EXPECT_EQ(response.error().message, R"(Cannot connect to "203.0.113.10")");
}

TEST(HttpClientUrl, invalidRequestHeader_IsRejectedBeforeConnecting) {
    auto factory = factory_for({});
    const Options options = options_for(factory);
    Request request;
    request.headers.emplace("Invalid Header", "value");

    const auto response = exchange(options, "http://203.0.113.10/dns-query", request);

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::INVALID_REQUEST);
    EXPECT_EQ(factory->tcp_calls, 0);
}

// ---------------------------------------------------------------------------
// Client: the redirect loop
// ---------------------------------------------------------------------------

TEST(HttpClientRedirect, sameOrigin_RebuildsTheRequestOnASecondConnection) {
    auto factory =
        factory_for({Behaviour::response("HTTP/1.1 302 Found\r\nLocation: /moved?q=1\r\nContent-Length: 0\r\n\r\n"),
                     Behaviour::response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")});
    const Options options = options_for(factory);

    const auto response = exchange(options, "http://203.0.113.10:8080/start", [] {
        Request request;
        request.headers.emplace("X-Trace", "abc");
        return request;
    }());

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status_, 200);
    EXPECT_EQ(response->text(), "ok");
    ASSERT_EQ(factory->tcp_calls, 2);
    // Every hop is a fresh connection to the same origin.
    EXPECT_TRUE(factory->sent_on(0).starts_with("GET /start HTTP/1.1\r\n"));
    EXPECT_TRUE(factory->sent_on(1).starts_with("GET /moved?q=1 HTTP/1.1\r\n"));
    EXPECT_NE(factory->sent_on(0).find("Host: 203.0.113.10:8080"), std::string::npos);
    EXPECT_NE(factory->sent_on(1).find("Host: 203.0.113.10:8080"), std::string::npos);
    // The application header survives the rebuild into the follow-up request.
    EXPECT_NE(factory->sent_on(1).find("X-Trace: abc"), std::string::npos);
}

TEST(HttpClientRedirect, crossOrigin_OpensAConnectionToTheNewOrigin) {
    auto factory =
        factory_for({Behaviour::response("HTTP/1.1 302 Found\r\nLocation: http://203.0.113.20:9090/elsewhere\r\n"
                                         "Content-Length: 0\r\n\r\n"),
                     Behaviour::response("HTTP/1.1 204 No Content\r\n\r\n")});
    Options options = options_for(factory);
    Request request;
    request.headers.emplace("Authorization", "Bearer secret");
    request.set_body("payload");

    const auto response = exchange(options, "http://203.0.113.10:8080/start", request);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status_, 204);
    ASSERT_EQ(factory->tcp_calls, 2);
    // 302 rewrites to GET, and the credential never leaves its origin.
    EXPECT_TRUE(factory->sent_on(1).starts_with("GET /elsewhere HTTP/1.1\r\n"));
    EXPECT_NE(factory->sent_on(1).find("Host: 203.0.113.20:9090"), std::string::npos);
    EXPECT_EQ(factory->sent_on(1).find("Authorization"), std::string::npos);
    EXPECT_EQ(factory->sent_on(1).find("payload"), std::string::npos);
}

TEST(HttpClientRedirect, crossOrigin_ToAnIpv6Literal_BracketsTheHost) {
    auto factory =
        factory_for({Behaviour::response(
                         "HTTP/1.1 302 Found\r\nLocation: http://[2001:db8::2]:8080/v6\r\nContent-Length: 0\r\n\r\n"),
                     Behaviour::response("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")});
    const Options options = options_for(factory);

    const auto response = exchange(options, "http://203.0.113.10:8080/start");

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status_, 200);
    ASSERT_EQ(factory->tcp_calls, 2);
    // The reassembled URL is absolute, so the IPv6 literal must stay bracketed
    // for the next hop's URI parse.
    EXPECT_NE(factory->sent_on(1).find("Host: [2001:db8::2]:8080"), std::string::npos);
}

TEST(HttpClientRedirect, temporaryRedirect_ReplaysTheMethodAndBody) {
    auto factory = factory_for(
        {Behaviour::response("HTTP/1.1 307 Temporary Redirect\r\nLocation: /retry\r\nContent-Length: 0\r\n\r\n"),
         Behaviour::response("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")});
    const Options options = options_for(factory);
    Request request;
    request.method = http::Method::POST;
    request.set_body("payload");
    request.content_type = "application/dns-message";

    const auto response = exchange(options, "http://203.0.113.10:8080/start", request);

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(factory->tcp_calls, 2);
    EXPECT_TRUE(factory->sent_on(1).starts_with("POST /retry HTTP/1.1\r\n"));
    EXPECT_NE(factory->sent_on(1).find("Content-Length: 7"), std::string::npos);
    EXPECT_NE(factory->sent_on(1).find("Content-Type: application/dns-message"), std::string::npos);
    EXPECT_NE(factory->sent_on(1).find("payload"), std::string::npos);
}

TEST(HttpClientRedirect, beyondTheLimit_ReportsTheLimit) {
    auto factory =
        factory_for({Behaviour::response("HTTP/1.1 302 Found\r\nLocation: /one\r\nContent-Length: 0\r\n\r\n"),
                     Behaviour::response("HTTP/1.1 302 Found\r\nLocation: /two\r\nContent-Length: 0\r\n\r\n")});
    Options options = options_for(factory);
    options.max_redirects = 1;

    const auto response = exchange(options, "http://203.0.113.10:8080/start");

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::REDIRECT_LIMIT_EXCEEDED);
    // The limit is checked before the second hop is followed, so only the two
    // redirects that produced the decision were ever fetched.
    EXPECT_EQ(factory->tcp_calls, 2);
}

TEST(HttpClientRedirect, disabledFollowing_LeavesTheRedirectResponse) {
    auto factory =
        factory_for({Behaviour::response("HTTP/1.1 302 Found\r\nLocation: /one\r\nContent-Length: 4\r\n\r\nbody")});
    Options options = options_for(factory);
    options.follow_redirects = false;

    const auto response = exchange(options, "http://203.0.113.10:8080/start");

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status_, 302);
    EXPECT_EQ(response->text(), "body");
    EXPECT_EQ(factory->tcp_calls, 1);
}

// ---------------------------------------------------------------------------
// the transient hop runs the session pipeline
// ---------------------------------------------------------------------------

TEST(HttpClientHop, lostFirstHop_IsReplayedOnARebuiltConnection) {
    // A transient hop runs on a one-shot session, so a connection lost before
    // the peer answered is recovered from for an idempotent request exactly as
    // a persistent hop would be.
    auto factory =
        factory_for({Behaviour::response(""), Behaviour::response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")});
    const Options options = options_for(factory);

    const auto response = exchange(options, "http://203.0.113.10:8080/start");

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status_, 200);
    EXPECT_EQ(response->text(), "ok");
    EXPECT_EQ(factory->tcp_calls, 2);
}

TEST(HttpClientHop, lostSecondHop_IsReplayedToo) {
    // The redirect budget is unchanged; the follow-up hop keeps its own
    // connection and its own recovery.
    auto factory =
        factory_for({Behaviour::response("HTTP/1.1 302 Found\r\nLocation: /moved\r\nContent-Length: 0\r\n\r\n"),
                     Behaviour::response(""), Behaviour::response("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")});
    const Options options = options_for(factory);

    const auto response = exchange(options, "http://203.0.113.10:8080/start");

    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->status_, 200);
    EXPECT_EQ(factory->tcp_calls, 3);
    EXPECT_TRUE(factory->sent_on(2).starts_with("GET /moved HTTP/1.1\r\n"));
}

TEST(HttpClientHop, nonIdempotentHop_IsNotReplayed) {
    // The replay rule is unchanged for a body-carrying request: a POST that may
    // have reached the server is not repeated.
    auto factory = factory_for({Behaviour::response("")});
    const Options options = options_for(factory);
    Request request;
    request.method = http::Method::POST;

    const auto response = exchange(options, "http://203.0.113.10:8080/start", request);

    ASSERT_FALSE(response.has_value());
    EXPECT_EQ(response.error().code, ErrorCode::CONNECTION_LOST);
    EXPECT_EQ(factory->tcp_calls, 1);
}

// ---------------------------------------------------------------------------
// connect_stream: the per-candidate connect policy
// ---------------------------------------------------------------------------

TEST(HttpConnectPaths, noConnectBudget_UsesTheUnboundedAttempt) {
    auto factory = factory_for({Behaviour::response("")});
    Options options = options_for(factory);
    options.connect_timeout = 0ms;
    const std::vector<domain::InetAddress> addresses{address_of("203.0.113.10")};

    const auto stream = coro::run(http::connect_stream("http", "203.0.113.10", addresses, 8080, options));

    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(factory->tcp_calls, 1);
}

TEST(HttpConnectPaths, noConnectBudget_AndAFailedAttempt_ReportsConnectFailed) {
    auto factory = factory_for({Behaviour{.connect_results = {false}}});
    const Options options = options_for(factory);
    const std::vector<domain::InetAddress> addresses{address_of("203.0.113.10")};

    const auto stream = coro::run(http::connect_stream("http", "203.0.113.10", addresses, 8080, options));

    ASSERT_FALSE(stream.has_value());
    EXPECT_EQ(stream.error().code, ErrorCode::CONNECT_FAILED);
    EXPECT_EQ(stream.error().message, R"(Cannot connect to "203.0.113.10")");
}

TEST(HttpConnectPaths, failedAttemptInsideTheBudget_ReportsConnectFailed) {
    auto factory = factory_for({Behaviour{.connect_results = {false}}});
    Options options = options_for(factory);
    options.connect_timeout = 50ms;
    const std::vector<domain::InetAddress> addresses{address_of("203.0.113.10")};
    coro::ManualClock clock;
    coro::Loop loop{clock};

    const auto stream = coro::run(loop, http::connect_stream("http", "203.0.113.10", addresses, 8080, options));
    EXPECT_EQ(clock.now(), coro::TimePoint{});

    ASSERT_FALSE(stream.has_value());
    EXPECT_EQ(stream.error().code, ErrorCode::CONNECT_FAILED);
    // A refusal inside the budget is not a timeout: the message names the host.
    EXPECT_EQ(stream.error().message, R"(Cannot connect to "203.0.113.10")");
}

TEST(HttpConnectPaths, severalCandidates_AFailedOneFallsThroughToTheNext) {
    auto factory = factory_for({Behaviour{.connect_results = {false}}, Behaviour::response("")});
    const Options options = options_for(factory);
    const std::vector<domain::InetAddress> addresses{address_of("203.0.113.10"), address_of("203.0.113.11"),
                                                     address_of("203.0.113.12")};

    const auto stream = coro::run(http::connect_stream("http", "203.0.113.10", addresses, 8080, options));

    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(factory->tcp_calls, 2);
}

TEST(HttpConnectPaths, noCandidates_ReportsThatThereIsNothingToConnectTo) {
    auto factory = factory_for({});
    const Options options = options_for(factory);

    const auto stream = coro::run(http::connect_stream("http", "example.test", {}, 443, options));

    ASSERT_FALSE(stream.has_value());
    EXPECT_EQ(stream.error().code, ErrorCode::CONNECT_FAILED);
    EXPECT_EQ(stream.error().message, "no address to connect to");
    EXPECT_EQ(factory->tcp_calls, 0);
}

TEST(HttpConnectPaths, timedOutCandidate_SkipsToTheNext) {
    // On a ManualClock the per-address budget expires while the first
    // candidate is still parked, which is what makes it a skipped candidate
    // rather than the answer.
    auto factory = factory_for({Behaviour{.connect_stall = 1h}, Behaviour::response("")});
    Options options = options_for(factory);
    options.connect_timeout = 10ms;
    const std::vector<domain::InetAddress> addresses{address_of("203.0.113.10"), address_of("203.0.113.11")};
    coro::ManualClock clock;
    coro::Loop loop{clock};

    const auto stream = coro::run(loop, http::connect_stream("http", "203.0.113.10", addresses, 8080, options));

    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(factory->tcp_calls, 2);
    // The loop only ever jumped as far as the per-address budget.
    EXPECT_EQ(clock.now(), (coro::TimePoint{10ms}));
}

TEST(HttpConnectPaths, everyCandidateTimesOut_ReportsTheLastTimeout) {
    auto factory = factory_for({Behaviour{.connect_stall = 1h}});
    Options options = options_for(factory);
    options.connect_timeout = 10ms;
    const std::vector<domain::InetAddress> addresses{address_of("203.0.113.10")};
    coro::ManualClock clock;
    coro::Loop loop{clock};

    const auto stream = coro::run(loop, http::connect_stream("http", "203.0.113.10", addresses, 8080, options));

    ASSERT_FALSE(stream.has_value());
    EXPECT_EQ(stream.error().code, ErrorCode::CONNECT_FAILED);
    EXPECT_EQ(stream.error().message, R"(Connection to "203.0.113.10" timed out)");
}

}  // namespace
