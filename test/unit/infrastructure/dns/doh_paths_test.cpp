//
// Unit tests for the DoH resolver's remaining local paths (src/infrastructure/
// dns/resolver/doh.cpp): the close() contract and the HTTP-to-DNS mapping of an
// endpoint whose host the client cannot reassemble into a URL.
//
// Everything runs on an injected in-memory stream factory: no socket is opened
// and no name is resolved. The classic exchange cases below pin the other half
// of the gap — that dns::detail rejects a malformed local query before it
// reaches the socket.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; the coroutine bodies here use EXPECT_* only.
//

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "coro/coro.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/exchange.h"
#include "infrastructure/dns/resolver/doh.h"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/stream.h"

namespace {

/// An IP-literal endpoint: the URL host bypasses the injected resolver.
constexpr const char* DOH_URL = "https://203.0.113.10/dns-query";

/// Answers the one DoH query it is sent with a minimal well-formed A record and
/// then reports EOF. The connection is keep-alive, so the session still owns it
/// when the exchange returns — which is what makes close() observable.
class DoHEchoStream final : public net::Stream {
public:
    explicit DoHEchoStream(int& closed) : closed_(closed) {}

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> ensure_connected() override { co_return {}; }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> send_all(std::span<const std::uint8_t> bytes) override {
        // The wire-format query is the HTTP body, past the header terminator.
        const std::string_view head(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const std::span<const std::uint8_t> query = bytes.subspan(head.find("\r\n\r\n") + 4);
        const auto id = static_cast<std::uint16_t>((static_cast<std::uint16_t>(query[0]) << 8) | query[1]);
        auto answer = dns::QueryBuilder{}.id(id).qr(true).ra(true).add_question("x.test", dns::RecordType::A).build();
        answer[7] = 1;  // ANCOUNT
        answer.insert(answer.end(), {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 0, 2, 42});
        response_ = "HTTP/1.1 200 OK\r\nContent-Type: application/dns-message\r\nContent-Length: " +
                    std::to_string(answer.size()) + "\r\n\r\n";
        response_.append(reinterpret_cast<const char*>(answer.data()), answer.size());
        co_return {};
    }

    [[nodiscard]] coro::Task<std::expected<std::size_t, net::IoError>> read_some(std::span<std::uint8_t> out) override {
        const std::size_t count = std::min(out.size(), response_.size() - cursor_);
        std::memcpy(out.data(), response_.data() + cursor_, count);
        cursor_ += count;
        co_return count;
    }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> read_exact(std::span<std::uint8_t> out) override {
        const auto read = co_await read_some(out);
        if (!read || *read != out.size()) {
            co_return std::unexpected(net::IoError::CONNECTION_FAILED);
        }
        co_return {};
    }

    void close() noexcept override {
        if (connected_) {
            ++closed_;
            connected_ = false;
        }
    }

    [[nodiscard]] bool connected() const noexcept override { return connected_; }

private:
    int& closed_;
    bool connected_{true};
    std::string response_;
    std::size_t cursor_{0};
};

/// A fresh DoHEchoStream per dial, counting dials and releases.
class EchoFactory final : public net::StreamFactory {
public:
    [[nodiscard]] std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&, const net::TlsOptions&,
                                                          std::shared_ptr<const net::TlsContext>) override {
        ++tls_calls;
        return std::make_unique<DoHEchoStream>(closed);
    }

    [[nodiscard]] std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&) override {
        return std::make_unique<DoHEchoStream>(closed);
    }

    int tls_calls{0};
    int closed{0};
};

// ---------------------------------------------------------------------------
// DoH: close() is safe on an idle resolver and owns no connection to release
// ---------------------------------------------------------------------------

TEST(DohResolver, close_IdleResolver_OpensNoStreamAndIsIdempotent) {
    auto factory = std::make_shared<EchoFactory>();
    http::Options options;
    options.factory = factory;
    dns::DohResolver resolver{DOH_URL, std::move(options)};

    resolver.close();
    resolver.close();

    EXPECT_EQ(factory->tls_calls, 0);
    EXPECT_EQ(factory->closed, 0);
}

TEST(DohResolver, close_AfterKeptAliveAnswer_DropsTheConnectionAndTheNextQueryRedials) {
    auto factory = std::make_shared<EchoFactory>();
    http::Options options;
    options.factory = factory;
    dns::DohResolver resolver{DOH_URL, std::move(options)};
    int answered_before_close = 0;
    int answered_after_close = 0;
    int closed_before_close = 0;

    coro::run([&]() -> coro::Task<void> {
        if ((co_await resolver.query("x.test", domain::RecordKind::A)).has_value()) {
            ++answered_before_close;
        }
        closed_before_close = factory->closed;
        resolver.close();
        if ((co_await resolver.query("x.test", domain::RecordKind::A)).has_value()) {
            ++answered_after_close;
        }
    }());

    // A keep-alive answer leaves the connection with the session, so close()
    // releases a live one instead of nothing.
    EXPECT_EQ(answered_before_close, 1);
    EXPECT_EQ(closed_before_close, 0);
    EXPECT_EQ(factory->closed, 1);
    EXPECT_EQ(answered_after_close, 1);
    EXPECT_EQ(factory->tls_calls, 2);
}

// ---------------------------------------------------------------------------
// DoH: an endpoint the client cannot reassemble is a local configuration defect
// ---------------------------------------------------------------------------

TEST(DohResolver, query_EndpointHostRejectedByTheReassembledUrl_IsAConfigError) {
    // "x:y" is not an IPv6 literal, so Uri::parse accepts it as a bare host
    // carrying port 8443 — but the client's own reassembly of that origin,
    // "https://[x:y]:8443/dns-query", does not parse. The endpoint itself is
    // misconfigured, so the verdict is CONFIG rather than a transient endpoint
    // failure, and no stream is ever requested.
    auto factory = std::make_shared<EchoFactory>();
    http::Options options;
    options.factory = factory;
    dns::DohResolver resolver{"https://x:y:8443/dns-query", std::move(options)};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONFIG);
    EXPECT_EQ(result.error().message, "invalid request URL");
    EXPECT_EQ(factory->tls_calls, 0);
}

// ---------------------------------------------------------------------------
// Classic exchange: a malformed local query never reaches a socket
// ---------------------------------------------------------------------------

/// A 12-octet header with QDCOUNT = 0: enough bytes for the header check, not a
/// question. The exchanges own their sockets, so a case whose verdict is
/// INVALID_QUERY is also the proof that none was opened.
constexpr std::array<std::uint8_t, 12> ZERO_QUESTION_HEADER{};

TEST(ClassicExchange, query_udp_HeaderWithoutQuestion_IsInvalidQueryBeforeAnySocket) {
    const auto server = *domain::InetAddress::parse("203.0.113.10");

    const auto result = coro::run(dns::detail::query_udp(server, 53, ZERO_QUESTION_HEADER));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::INVALID_QUERY);
}

TEST(ClassicExchange, query_classic_HeaderWithoutQuestion_IsInvalidQueryWithoutTcpFallback) {
    // The attempt budget also wraps the UDP→TCP fallback; a query that never
    // becomes wire traffic must be rejected without entering it.
    const auto server = *domain::InetAddress::parse("203.0.113.10");

    const auto result = coro::run(dns::detail::query_classic(server, 53, ZERO_QUESTION_HEADER));

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::INVALID_QUERY);
}

}  // namespace