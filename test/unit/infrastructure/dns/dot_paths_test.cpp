//
// Unit tests for the DoT resolver branches that no exchange has to reach
// (src/infrastructure/dns/resolver/dot.cpp): the endpoint contract enforced at
// construction, the SNI default, and close() on a resolver that owns no query.
//
// Everything runs on an injected in-memory stream factory with an injected (or
// literal) endpoint lookup: no socket is opened and no name is resolved.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; the coroutine body here uses EXPECT_* only.
//

#include <algorithm>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "coro/coro.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/resolver/dot.h"
#include "infrastructure/dns/types.h"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/network/address/host_resolver.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/stream.h"

namespace domain {
enum class AddressFamily;
}  // namespace domain

namespace {

using ResolveResult = std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>;

/// The endpoint host every stream-factory case dials.
[[nodiscard]] domain::InetAddress endpoint_address() {
    const auto address = domain::InetAddress::parse("203.0.113.10");
    EXPECT_TRUE(address.has_value());
    return address.value_or(domain::InetAddress{});
}

/// Answers the one DoT query it is sent with a minimal well-formed A record and
/// then reports EOF. The connection is keep-alive, so the resolver still owns it
/// when the exchange returns — which is what makes close() observable.
class DotEchoStream final : public net::Stream {
public:
    explicit DotEchoStream(int& closed) : closed_(closed) {}

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> ensure_connected() override { co_return {}; }

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> send_all(std::span<const std::uint8_t> bytes) override {
        // DoT frames the query behind a two-octet big-endian length prefix.
        const std::span<const std::uint8_t> query = bytes.subspan(2);
        const auto id = static_cast<std::uint16_t>((static_cast<std::uint16_t>(query[0]) << 8) | query[1]);
        auto answer = dns::QueryBuilder{}.id(id).qr(true).ra(true).add_question("x.test", dns::RecordType::A).build();
        answer[7] = 1;  // ANCOUNT
        answer.insert(answer.end(), {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 0, 2, 42});
        response_.push_back(static_cast<char>(static_cast<std::uint8_t>(answer.size() >> 8)));
        response_.push_back(static_cast<char>(static_cast<std::uint8_t>(answer.size())));
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

/// A fresh DotEchoStream per dial, counting dials, releases and the SNI
/// identity each dial was given.
class EchoFactory final : public net::StreamFactory {
public:
    [[nodiscard]] std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&,
                                                          const net::TlsOptions& tls_options,
                                                          std::shared_ptr<const net::TlsContext>) override {
        ++tls_calls;
        sni = tls_options.sni_hostname;
        return std::make_unique<DotEchoStream>(closed);
    }

    [[nodiscard]] std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&) override {
        return std::make_unique<DotEchoStream>(closed);
    }

    int tls_calls{0};
    int closed{0};
    std::optional<std::string> sni{};
};

/// An endpoint lookup that always answers with one fixed address, so a named
/// endpoint never reaches bootstrap DNS.
[[nodiscard]] net::HostResolver fixed_address(domain::InetAddress address) {
    return [address](std::string, std::optional<domain::AddressFamily>) -> coro::Task<ResolveResult> {
        co_return std::vector<domain::InetAddress>{address};
    };
}

// ---------------------------------------------------------------------------
// DoT: the endpoint contract is enforced at construction, before any I/O
// ---------------------------------------------------------------------------

TEST(DotResolver, ctor_EndpointWithoutAName_ThrowsBeforeAnyStreamIsRequested) {
    auto factory = std::make_shared<EchoFactory>();
    dns::EndpointOptions options;
    options.factory = factory;

    EXPECT_THROW({ dns::DotResolver resolver("", 853, std::move(options)); }, std::invalid_argument);
    EXPECT_EQ(factory->tls_calls, 0);
}

TEST(DotResolver, ctor_EndpointWithEmbeddedNul_ThrowsBeforeAnyStreamIsRequested) {
    auto factory = std::make_shared<EchoFactory>();
    dns::EndpointOptions options;
    options.factory = factory;

    // A NUL would truncate the SNI identity and the bootstrap lookup alike, so
    // it is a configuration defect, not a name to resolve.
    EXPECT_THROW(
        { dns::DotResolver resolver(std::string{"dot\0.test", 9}, 853, std::move(options)); }, std::invalid_argument);
    EXPECT_EQ(factory->tls_calls, 0);
}

TEST(DotResolver, ctor_EndpointOnPortZero_ThrowsBeforeAnyStreamIsRequested) {
    auto factory = std::make_shared<EchoFactory>();
    dns::EndpointOptions options;
    options.factory = factory;

    EXPECT_THROW({ dns::DotResolver resolver("203.0.113.10", 0, std::move(options)); }, std::invalid_argument);
    EXPECT_EQ(factory->tls_calls, 0);
}

TEST(DotResolver, ctor_NamedEndpoint_DefaultsTheSniIdentityToTheEndpointHost) {
    auto factory = std::make_shared<EchoFactory>();
    dns::EndpointOptions options;
    options.factory = factory;
    options.resolve = fixed_address(endpoint_address());
    dns::DotResolver resolver{"dot.test", 853, std::move(options)};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(factory->tls_calls, 1);
    ASSERT_TRUE(factory->sni.has_value());
    EXPECT_EQ(*factory->sni, "dot.test");
}

TEST(DotResolver, ctor_PreconfiguredSniIdentity_IsKeptInsteadOfTheEndpointHost) {
    auto factory = std::make_shared<EchoFactory>();
    dns::EndpointOptions options;
    options.factory = factory;
    options.resolve = fixed_address(endpoint_address());
    // The assembly layer may pin the identity (a CNAME'd endpoint, or a
    // certificate issued for a different name). The resolver must not second-guess it.
    options.tls.sni_hostname = "pinned.example";
    dns::DotResolver resolver{"dot.test", 853, std::move(options)};

    const auto result = coro::run(resolver.query("x.test", domain::RecordKind::A));

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(factory->tls_calls, 1);
    ASSERT_TRUE(factory->sni.has_value());
    EXPECT_EQ(*factory->sni, "pinned.example");
}

// ---------------------------------------------------------------------------
// DoT: close() releases a connection the resolver actually owns
// ---------------------------------------------------------------------------

TEST(DotResolver, close_IdleResolver_ReleasesNothingAndIsIdempotent) {
    auto factory = std::make_shared<EchoFactory>();
    dns::EndpointOptions options;
    options.factory = factory;
    dns::DotResolver resolver{"203.0.113.10", 853, std::move(options)};

    resolver.close();
    resolver.close();

    EXPECT_EQ(factory->tls_calls, 0);
    EXPECT_EQ(factory->closed, 0);
}

TEST(DotResolver, close_AfterKeptAliveAnswer_DropsTheConnectionAndTheNextQueryRedials) {
    auto factory = std::make_shared<EchoFactory>();
    dns::EndpointOptions options;
    options.factory = factory;
    dns::DotResolver resolver{"203.0.113.10", 853, std::move(options)};
    int answered_before_close = 0;
    int answered_after_close = 0;
    int closed_before_close = 0;

    coro::run([&]() -> coro::Task<void> {
        const auto before = co_await resolver.query("x.test", domain::RecordKind::A);
        if (before.has_value()) {
            ++answered_before_close;
        }
        closed_before_close = factory->closed;
        resolver.close();
        const auto after = co_await resolver.query("x.test", domain::RecordKind::A);
        if (after.has_value()) {
            ++answered_after_close;
        }
    }());

    // A keep-alive answer leaves the stream with the resolver, so close()
    // releases a live connection instead of nothing.
    EXPECT_EQ(answered_before_close, 1);
    EXPECT_EQ(closed_before_close, 0);
    EXPECT_EQ(factory->closed, 1);
    EXPECT_EQ(answered_after_close, 1);
    EXPECT_EQ(factory->tls_calls, 2);
}

}  // namespace