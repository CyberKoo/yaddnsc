//
// Unit tests for net::http::Session (src/infrastructure/network/http/session.cpp).
//
// The persistent-connection policy is exercised through a scripted
// in-memory StreamFactory (no network I/O):
//   - scheme -> factory pairing (http -> create_tcp, https -> create_tls)
//   - connect/handshake error mapping (cancelled / timeout / failed)
//   - connection-loss retry: idempotent methods retried exactly once on a
//     rebuilt connection, non-idempotent methods not retried,
//     non-recoverable errors not retried
//   - stream reuse across exchanges
// =============================================================================

#include "infrastructure/network/http/session.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/protocol/wire.h"
#include "infrastructure/network/http/stream_factory.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/stream.h"
#include "support/util/cancellation_token.hpp"

using net::http::ErrorCode;
using net::http::Method;
using Transport::IoError;

namespace {

/// Scripted in-memory Transport::Stream.
///
/// Feeds `input` to readers, records everything written, can fail reads
/// with a fixed error (exchange() maps IoError::CONNECTION_FAILED to
/// ErrorCode::CONNECTION_LOST, which drives the session retry policy) and
/// can fail ensure_connected() with a fixed error.
class FakeStream final : public Transport::Stream {
public:
    std::string input;
    std::optional<IoError> fail_reads;
    std::optional<IoError> connect_error;
    std::string sent;

    [[nodiscard]] std::expected<void, IoError> ensure_connected(const Utils::CancellationToken&) override {
        if (connect_error) {
            return std::unexpected(*connect_error);
        }
        return {};
    }

    void close() noexcept override {}

    [[nodiscard]] std::expected<size_t, IoError> read_some(std::span<std::uint8_t> buf,
                                                           const Utils::CancellationToken&) override {
        if (fail_reads) {
            return std::unexpected(*fail_reads);
        }
        if (pos_ >= input.size()) {
            return std::unexpected(IoError::CONNECTION_FAILED);  // EOF
        }
        const size_t n = std::min(buf.size(), input.size() - pos_);
        std::memcpy(buf.data(), input.data() + pos_, n);
        pos_ += n;
        return n;
    }

    [[nodiscard]] std::expected<void, IoError> read_exact(std::span<std::uint8_t> buf,
                                                          const Utils::CancellationToken& token) override {
        auto remaining = buf;
        while (!remaining.empty()) {
            auto n = read_some(remaining, token);
            if (!n) {
                return std::unexpected(n.error());
            }
            remaining = remaining.subspan(*n);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, IoError> send_all(std::span<const std::uint8_t> data,
                                                        const Utils::CancellationToken&) override {
        sent.append(reinterpret_cast<const char*>(data.data()), data.size());
        return {};
    }

private:
    size_t pos_ = 0;
};

/// Factory serving scripted streams per host, recording every call.
class FakeFactory final : public net::http::StreamFactory {
public:
    [[nodiscard]] std::unique_ptr<Transport::Stream> create_tls(std::string_view host,
                                                                std::uint16_t /*port*/,
                                                                const Transport::Options& /*conn_opts*/,
                                                                const Transport::TlsOptions& /*tls_opts*/) override {
        tls_hosts.emplace_back(host);
        return next(tls_streams);
    }

    [[nodiscard]] std::unique_ptr<Transport::Stream> create_tcp(std::string_view host,
                                                                std::uint16_t /*port*/,
                                                                const Transport::Options& /*opts*/) override {
        tcp_hosts.emplace_back(host);
        return next(tcp_streams);
    }

    std::deque<std::unique_ptr<FakeStream>> tcp_streams;
    std::deque<std::unique_ptr<FakeStream>> tls_streams;
    std::vector<std::string> tcp_hosts;
    std::vector<std::string> tls_hosts;

private:
    static std::unique_ptr<Transport::Stream> next(std::deque<std::unique_ptr<FakeStream>>& streams) {
        if (streams.empty()) {
            return std::make_unique<FakeStream>();  // blank stream: EOF on read
        }
        auto stream = std::move(streams.front());
        streams.pop_front();
        return stream;
    }
};

std::unique_ptr<FakeStream> ok_stream() {
    auto stream = std::make_unique<FakeStream>();
    stream->input = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nX-Marker: yes\r\n\r\nok";
    return stream;
}

net::http::Session make_session(std::shared_ptr<FakeFactory> factory, std::string scheme) {
    return net::http::Session(std::move(factory), {}, {}, std::move(scheme), "example.com", 80, net::http::Limits{});
}

net::http::protocol::WireRequest get_request() {
    return {.method = Method::GET, .target = "/", .headers = {{"Host", "example.com"}}};
}

}  // namespace

// ── scheme / factory pairing ─────────────────────────────────────────────────

TEST(HttpSession, HttpScheme_UsesTcpFactory) {
    auto factory = std::make_shared<FakeFactory>();
    factory->tcp_streams.push_back(ok_stream());

    auto session = make_session(factory, "http");
    const auto resp = session.exchange(get_request(), {});

    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->text(), "ok");
    EXPECT_EQ(resp->headers.count("X-Marker"), 1);
    ASSERT_EQ(factory->tcp_hosts.size(), 1);
    EXPECT_EQ(factory->tcp_hosts.front(), "example.com");
    EXPECT_TRUE(factory->tls_hosts.empty());
}

TEST(HttpSession, KeepAliveMaxRebuildsBeforeTheNextExchange) {
    auto factory = std::make_shared<FakeFactory>();
    auto limited = std::make_unique<FakeStream>();
    limited->input =
        "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nKeep-Alive: timeout=5, max=1\r\nContent-Length: 2\r\n\r\nok";
    factory->tcp_streams.push_back(std::move(limited));
    factory->tcp_streams.push_back(ok_stream());

    auto session = make_session(factory, "http");
    ASSERT_TRUE(session.exchange(get_request(), {}));
    ASSERT_TRUE(session.exchange(get_request(), {}));
    EXPECT_EQ(factory->tcp_hosts.size(), 2);
}

TEST(HttpSession, RepeatedKeepAliveMaxDoesNotResetTheConnectionCap) {
    auto factory = std::make_shared<FakeFactory>();
    auto limited = std::make_unique<FakeStream>();
    limited->input =
        "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nKeep-Alive: max=2\r\nContent-Length: 2\r\n\r\nok"
        "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\nKeep-Alive: max=2\r\nContent-Length: 2\r\n\r\nok";
    factory->tcp_streams.push_back(std::move(limited));
    factory->tcp_streams.push_back(ok_stream());

    auto session = make_session(factory, "http");
    ASSERT_TRUE(session.exchange(get_request(), {}));
    ASSERT_TRUE(session.exchange(get_request(), {}));
    ASSERT_TRUE(session.exchange(get_request(), {}));
    EXPECT_EQ(factory->tcp_hosts.size(), 2);
}

TEST(HttpSession, HttpsScheme_UsesTlsFactory) {
    auto factory = std::make_shared<FakeFactory>();
    factory->tls_streams.push_back(ok_stream());

    auto session = make_session(factory, "https");
    const auto resp = session.exchange(get_request(), {});

    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    ASSERT_EQ(factory->tls_hosts.size(), 1);
    EXPECT_EQ(factory->tls_hosts.front(), "example.com");
    EXPECT_TRUE(factory->tcp_hosts.empty());
}

// ── connect / handshake error mapping ────────────────────────────────────────

TEST(HttpSession, ConnectError_MappedToDomainError) {
    auto factory = std::make_shared<FakeFactory>();

    auto cancelled = std::make_unique<FakeStream>();
    cancelled->connect_error = IoError::CANCELLED;
    factory->tcp_streams.push_back(std::move(cancelled));
    auto session = make_session(factory, "http");
    const auto r1 = session.exchange(get_request(), {});
    ASSERT_FALSE(r1);
    EXPECT_EQ(r1.error().code, ErrorCode::CANCELLED);

    auto timed_out = std::make_unique<FakeStream>();
    timed_out->connect_error = IoError::TIMEOUT;
    factory->tcp_streams.push_back(std::move(timed_out));
    const auto r2 = session.exchange(get_request(), {});
    ASSERT_FALSE(r2);
    EXPECT_EQ(r2.error().code, ErrorCode::TIMEOUT);

    auto refused = std::make_unique<FakeStream>();
    refused->connect_error = IoError::CONNECTION_FAILED;
    factory->tcp_streams.push_back(std::move(refused));
    const auto r3 = session.exchange(get_request(), {});
    ASSERT_FALSE(r3);
    EXPECT_EQ(r3.error().code, ErrorCode::CONNECT_FAILED);
}

// ── connection-loss retry policy ─────────────────────────────────────────────

TEST(HttpSession, IdempotentRequest_ConnectionLost_RetriedOnceOnNewStream) {
    auto factory = std::make_shared<FakeFactory>();

    auto dying = std::make_unique<FakeStream>();
    dying->fail_reads = IoError::CONNECTION_FAILED;  // exchange -> CONNECTION_LOST
    factory->tcp_streams.push_back(std::move(dying));
    factory->tcp_streams.push_back(ok_stream());

    auto session = make_session(factory, "http");
    const auto resp = session.exchange(get_request(), {});

    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->status, 200);
    EXPECT_EQ(resp->text(), "ok");
    EXPECT_EQ(factory->tcp_hosts.size(), 2);  // rebuilt connection
}

TEST(HttpSession, IdempotentRequest_RetryFails_ReturnsConnectionLost) {
    auto factory = std::make_shared<FakeFactory>();

    auto dying1 = std::make_unique<FakeStream>();
    dying1->fail_reads = IoError::CONNECTION_FAILED;
    auto dying2 = std::make_unique<FakeStream>();
    dying2->fail_reads = IoError::CONNECTION_FAILED;
    factory->tcp_streams.push_back(std::move(dying1));
    factory->tcp_streams.push_back(std::move(dying2));

    auto session = make_session(factory, "http");
    const auto resp = session.exchange(get_request(), {});

    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::CONNECTION_LOST);
    EXPECT_EQ(factory->tcp_hosts.size(), 2);
}

TEST(HttpSession, NonIdempotentRequest_ConnectionLost_NotRetried) {
    auto factory = std::make_shared<FakeFactory>();

    auto dying = std::make_unique<FakeStream>();
    dying->fail_reads = IoError::CONNECTION_FAILED;
    factory->tcp_streams.push_back(std::move(dying));
    factory->tcp_streams.push_back(ok_stream());  // must NOT be consumed

    auto session = make_session(factory, "http");
    auto req = get_request();
    req.method = Method::POST;
    req.body = "payload";
    const auto resp = session.exchange(req, {});

    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::CONNECTION_LOST);
    EXPECT_EQ(factory->tcp_hosts.size(), 1);  // no retry for POST
}

TEST(HttpSession, NonRecoverableError_NotRetried) {
    auto factory = std::make_shared<FakeFactory>();

    auto cancelling = std::make_unique<FakeStream>();
    cancelling->fail_reads = IoError::CANCELLED;  // -> ErrorCode::CANCELLED
    factory->tcp_streams.push_back(std::move(cancelling));
    factory->tcp_streams.push_back(ok_stream());  // must NOT be consumed

    auto session = make_session(factory, "http");
    const auto resp = session.exchange(get_request(), {});

    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::CANCELLED);
    EXPECT_EQ(factory->tcp_hosts.size(), 1);
}

TEST(HttpSession, ReconnectFailsOnRetry_ReturnsConnectError) {
    auto factory = std::make_shared<FakeFactory>();

    auto dying = std::make_unique<FakeStream>();
    dying->fail_reads = IoError::CONNECTION_FAILED;
    auto refused = std::make_unique<FakeStream>();
    refused->connect_error = IoError::TIMEOUT;
    factory->tcp_streams.push_back(std::move(dying));
    factory->tcp_streams.push_back(std::move(refused));

    auto session = make_session(factory, "http");
    const auto resp = session.exchange(get_request(), {});

    ASSERT_FALSE(resp);
    EXPECT_EQ(resp.error().code, ErrorCode::TIMEOUT);
    EXPECT_EQ(factory->tcp_hosts.size(), 2);
}

// ── stream reuse ─────────────────────────────────────────────────────────────

TEST(HttpSession, HealthyStream_ReusedAcrossExchanges) {
    auto factory = std::make_shared<FakeFactory>();

    // One scripted stream: the response is consumed by the first exchange.
    factory->tcp_streams.push_back(ok_stream());

    auto session = make_session(factory, "http");
    ASSERT_TRUE(session.exchange(get_request(), {}));

    // POST is not retried: the EOF surfaces as CONNECTION_LOST and the
    // factory must NOT be consulted again — the connection was reused.
    auto req = get_request();
    req.method = Method::POST;
    req.body = "x";
    const auto resp = session.exchange(req, {});

    ASSERT_FALSE(resp);  // stream exhausted -> EOF
    EXPECT_EQ(resp.error().code, ErrorCode::CONNECTION_LOST);
    EXPECT_EQ(factory->tcp_hosts.size(), 1);  // connection reused, not rebuilt
}

// ── DefaultStreamFactory ─────────────────────────────────────────────────────

TEST(HttpSession, DefaultStreamFactory_CreatesRealStreams) {
    net::http::DefaultStreamFactory factory;

    const auto tcp = factory.create_tcp("127.0.0.1", 80, {});
    EXPECT_NE(tcp, nullptr);

    const auto tls = factory.create_tls("127.0.0.1", 443, {}, {});
    EXPECT_NE(tls, nullptr);
}
