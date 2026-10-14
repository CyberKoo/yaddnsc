//
// IP-source paths — the interface, HTTP and adapter sources behind their public
// entry points (src/infrastructure/ip_source/{iface,http,adapter}.cpp).
//
// The interface source reads a live getifaddrs() snapshot through the loopback
// interface (a metadata read, no traffic), and the HTTP source runs on the
// scripted in-memory stream — the net::StreamFactory injection point — with an
// injected hostname resolver, so no socket is ever opened and no name is ever
// looked up. The adapter cases reuse both to pin the record-kind → address
// family mapping and the per-source construction.
//
// NOTE: ASSERT_* macros expand to `return;`, which is ill-formed inside a
// coroutine body; the injected resolver here uses EXPECT_* only.
//

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "coro/coro.h"
#include "domain/config/ip_source_kind.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/http/types.h"
#include "infrastructure/ip_source/adapter.h"
#include "infrastructure/ip_source/http.h"
#include "infrastructure/ip_source/iface.h"
#include "infrastructure/ip_source/iface_util.h"
#include "infrastructure/ip_source/result.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/options.h"
#include "infrastructure/network/transport/stream.h"

namespace domain {
struct DnsErrorInfo;
}  // namespace domain

namespace {

using IpSourceError = domain::IpSourceError;

/// Interface name no host has: get_addresses() answers std::nullopt for it.
constexpr std::string_view ABSENT_INTERFACE = "yaddnsc-none0";

/// Endpoint the scripted peer answers; the host is an IP literal so the HTTP
/// client never consults the injected resolver on these cases.
constexpr std::string_view SERVER_URL = "http://203.0.113.9/ip";

/// A one-line HTTP/1.1 response carrying `body`.
[[nodiscard]] std::string http_response(const std::string_view body) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string{body};
}

// ---------------------------------------------------------------------------
// Scripted transport
// ---------------------------------------------------------------------------

/// What one scripted connection does.
struct Behaviour {
    std::string incoming{};               ///< bytes the peer serves; empty means an immediate EOF
    std::vector<bool> connect_results{};  ///< ensure_connected() results, consumed in order
};

/// A scripted in-memory stream: serves `behaviour` and records what was written.
class ScriptedStream final : public net::Stream {
public:
    explicit ScriptedStream(Behaviour behaviour) : behaviour_(std::move(behaviour)) {}

    [[nodiscard]] coro::Task<std::expected<void, net::IoError>> ensure_connected() override {
        const std::size_t index = connect_calls_++;
        const bool ok = index < behaviour_.connect_results.size() ? behaviour_.connect_results[index] : true;
        co_return ok ? std::expected<void, net::IoError>{} : std::unexpected(net::IoError::CONNECTION_FAILED);
    }

    [[nodiscard]] coro::Task<std::expected<std::size_t, net::IoError>> read_some(
        const std::span<std::uint8_t> buf) override {
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
        sent_.append(reinterpret_cast<const char*>(data.data()), data.size());
        co_return {};
    }

    void close() noexcept override {}

    [[nodiscard]] bool connected() const noexcept override { return true; }

    [[nodiscard]] const std::string& sent() const noexcept { return sent_; }

private:
    Behaviour behaviour_;
    std::size_t connect_calls_{0};
    std::size_t cursor_{0};
    std::string sent_;
};

/// Hands out scripted connections and records the connect policy it was handed,
/// which is how the source's address-family and interface overrides are observed.
class ScriptedFactory final : public net::StreamFactory {
public:
    explicit ScriptedFactory(Behaviour behaviour) : behaviour_(std::move(behaviour)) {}

    [[nodiscard]] std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions&, const net::TlsOptions&,
                                                          std::shared_ptr<const net::TlsContext>) override {
        ++tls_calls;
        return std::make_unique<ScriptedStream>(behaviour_);
    }

    [[nodiscard]] std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t,
                                                          const net::ConnectOptions& connect) override {
        ++tcp_calls;
        bound_interface = connect.interface;
        return std::make_unique<ScriptedStream>(behaviour_);
    }

    int tcp_calls{0};
    int tls_calls{0};
    std::optional<std::string> bound_interface{};
    Behaviour behaviour_;
};

/// The shared HTTP policy of a scripted source: the in-memory factory, no
/// per-address connect budget, and a resolver that records the family it was
/// asked for and answers with a fixed address.
[[nodiscard]] http::Options scripted_options(const std::shared_ptr<ScriptedFactory>& factory,
                                             std::optional<domain::AddressFamily>* asked_family = nullptr) {
    http::Options options;
    options.factory = factory;
    options.connect_timeout = std::chrono::milliseconds{0};
    options.resolve = [asked_family](std::string, std::optional<domain::AddressFamily> family)
        -> coro::Task<std::expected<std::vector<domain::InetAddress>, domain::DnsErrorInfo>> {
        if (asked_family != nullptr) {
            *asked_family = family;
        }
        co_return std::vector<domain::InetAddress>{*domain::InetAddress::parse("203.0.113.9")};
    };
    return options;
}

[[nodiscard]] ipsource::Result resolve_source(ipsource::HttpIpSource& source) {
    return coro::run(source.resolve());
}

// ---------------------------------------------------------------------------
// Interface helpers
// ---------------------------------------------------------------------------

/// The interface that owns 127.0.0.1 — "lo" on Linux, "lo0" on the BSDs.
[[nodiscard]] std::string loopback_interface() {
    for (const auto& entry : ipsource::get_all()) {
        const bool owns_loopback = std::ranges::any_of(entry.addresses, [](const domain::InetAddress& address) {
            const auto* v4 = address.as_v4();
            return v4 != nullptr && v4->to_string() == "127.0.0.1";
        });
        if (owns_loopback) {
            return entry.name;
        }
    }
    return {};
}

[[nodiscard]] std::vector<domain::InetAddress> keep_family(const std::vector<domain::InetAddress>& addresses,
                                                           const domain::AddressFamily family) {
    std::vector<domain::InetAddress> kept;
    for (const domain::InetAddress& address : addresses) {
        if (address.get_family() == family) {
            kept.push_back(address);
        }
    }
    return kept;
}

[[nodiscard]] bool every_address_is(const std::vector<domain::InetAddress>& addresses,
                                    const domain::AddressFamily family) {
    return std::ranges::all_of(addresses,
                               [family](const domain::InetAddress& address) { return address.get_family() == family; });
}

/// One lookup through InterfaceIpSource on a fresh loop.
[[nodiscard]] ipsource::Result resolve_interface(std::string interface_name, const domain::AddressFamily family) {
    ipsource::InterfaceIpSource source{std::move(interface_name), family};
    return coro::run(source.resolve());
}

[[nodiscard]] domain::SubdomainConfig interface_config(const domain::RecordKind type, std::string interface) {
    domain::SubdomainConfig config;
    config.type = type;
    config.ip_source = domain::IpSource::INTERFACE;
    config.interface = std::move(interface);
    return config;
}

// ---------------------------------------------------------------------------
// InterfaceIpSource
// ---------------------------------------------------------------------------

TEST(IpSourceInterface, AbsentInterfaceIsUnavailableAndNamesTheInterface) {
    if (ipsource::get_addresses(std::string{ABSENT_INTERFACE}).has_value()) {
        GTEST_SKIP() << "the probe interface name is in use";
    }

    const ipsource::Result result = resolve_interface(std::string{ABSENT_INTERFACE}, domain::AddressFamily::IPV4);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
    EXPECT_NE(result.error().message.find(ABSENT_INTERFACE), std::string::npos);
}

TEST(IpSourceInterface, UnspecifiedFamilyKeepsEveryAddressOfTheInterface) {
    const std::string loopback = loopback_interface();
    const auto known = ipsource::get_addresses(loopback);
    if (loopback.empty() || !known.has_value()) {
        GTEST_SKIP() << "no interface owns 127.0.0.1";
    }

    const ipsource::Result result = resolve_interface(loopback, domain::AddressFamily::UNSPECIFIED);

    ASSERT_TRUE(result.has_value());
    // No filtering at all: the whole snapshot survives, both families included.
    EXPECT_EQ(result->size(), known->size());
    EXPECT_EQ(result->size(), keep_family(*known, domain::AddressFamily::IPV4).size() +
                                  keep_family(*known, domain::AddressFamily::IPV6).size());
}

TEST(IpSourceInterface, Ipv4FamilyDropsEveryOtherFamily) {
    const std::string loopback = loopback_interface();
    const auto known = ipsource::get_addresses(loopback);
    if (loopback.empty() || !known.has_value()) {
        GTEST_SKIP() << "no interface owns 127.0.0.1";
    }

    const ipsource::Result result = resolve_interface(loopback, domain::AddressFamily::IPV4);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(every_address_is(*result, domain::AddressFamily::IPV4));
    EXPECT_EQ(result->size(), keep_family(*known, domain::AddressFamily::IPV4).size());
}

// ---------------------------------------------------------------------------
// HttpIpSource: the address-family / interface overrides applied at construction
// ---------------------------------------------------------------------------

TEST(IpSourceHttp, AddressFamilyReachesTheResolverOnlyWhenItIsSpecified) {
    std::optional<domain::AddressFamily> asked_family;
    auto factory = std::make_shared<ScriptedFactory>(Behaviour{.incoming = http_response("203.0.113.9")});
    ipsource::HttpIpSource source{std::string{"http://ip.example.test/ip"}, domain::AddressFamily::IPV4, "",
                                  scripted_options(factory, &asked_family)};

    const ipsource::Result result = resolve_source(source);

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(asked_family, std::optional{domain::AddressFamily::IPV4});
    // Nothing was pinned: an empty bind interface leaves the kernel's choice.
    EXPECT_EQ(factory->bound_interface, std::nullopt);
}

TEST(IpSourceHttp, UnspecifiedFamilyAndBindInterfaceLeaveTheSharedPolicy) {
    std::optional<domain::AddressFamily> asked_family;
    auto factory = std::make_shared<ScriptedFactory>(Behaviour{.incoming = http_response("203.0.113.9")});
    ipsource::HttpIpSource source{std::string{"http://ip.example.test/ip"}, domain::AddressFamily::UNSPECIFIED,
                                  "yaddnsc-bind0", scripted_options(factory, &asked_family)};

    const ipsource::Result result = resolve_source(source);

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(asked_family, std::nullopt);
    EXPECT_EQ(factory->bound_interface, std::optional<std::string>{"yaddnsc-bind0"});
}

// ---------------------------------------------------------------------------
// HttpIpSource: the response grammar
// ---------------------------------------------------------------------------

TEST(IpSourceHttp, OneAddressIsParsedOutOfTheTrimmedBody) {
    auto factory = std::make_shared<ScriptedFactory>(Behaviour{.incoming = http_response("  203.0.113.7\n")});
    ipsource::HttpIpSource source{std::string{SERVER_URL}, domain::AddressFamily::UNSPECIFIED, "",
                                  scripted_options(factory)};

    const ipsource::Result result = resolve_source(source);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->front().to_string(), "203.0.113.7");
    EXPECT_EQ(result->front().get_family(), domain::AddressFamily::IPV4);
}

TEST(IpSourceHttp, NonAddressBodyIsRejectedAsUnavailable) {
    auto factory = std::make_shared<ScriptedFactory>(Behaviour{.incoming = http_response("service unavailable")});
    ipsource::HttpIpSource source{std::string{SERVER_URL}, domain::AddressFamily::UNSPECIFIED, "",
                                  scripted_options(factory)};

    const ipsource::Result result = resolve_source(source);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
    EXPECT_EQ(result.error().message, "the IP source server did not return a valid IP address");
}

TEST(IpSourceHttp, AddressOfTheWrongFamilyIsRejected) {
    auto factory = std::make_shared<ScriptedFactory>(Behaviour{.incoming = http_response("2001:db8::1")});
    ipsource::HttpIpSource source{std::string{SERVER_URL}, domain::AddressFamily::IPV4, "", scripted_options(factory)};

    const ipsource::Result result = resolve_source(source);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
    EXPECT_EQ(result.error().message, "the IP source server returned an address of the wrong family");
}

TEST(IpSourceHttp, Ipv6BodyIsAcceptedWhenTheFamilyAsksForIt) {
    auto factory = std::make_shared<ScriptedFactory>(Behaviour{.incoming = http_response("2001:db8::1")});
    ipsource::HttpIpSource source{std::string{SERVER_URL}, domain::AddressFamily::IPV6, "", scripted_options(factory)};

    const ipsource::Result result = resolve_source(source);

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->front().get_family(), domain::AddressFamily::IPV6);
}

TEST(IpSourceHttp, TransportFailureIsReportedByTheServerRole) {
    auto factory = std::make_shared<ScriptedFactory>(Behaviour{.connect_results = {false}});
    ipsource::HttpIpSource source{std::string{SERVER_URL}, domain::AddressFamily::UNSPECIFIED, "",
                                  scripted_options(factory)};

    const ipsource::Result result = resolve_source(source);

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
    // The transport names the host; the cycle log names the role instead.
    EXPECT_EQ(result.error().message, "cannot connect to the IP source server");
}

// ---------------------------------------------------------------------------
// IpSourceAdapter: record kind → address family, per-source construction
// ---------------------------------------------------------------------------

TEST(IpSourceAdapter, RecordKindAAgainSelectsOnlyIpv4Addresses) {
    const std::string loopback = loopback_interface();
    const auto known = ipsource::get_addresses(loopback);
    if (loopback.empty() || !known.has_value()) {
        GTEST_SKIP() << "no interface owns 127.0.0.1";
    }
    ipsource::IpSourceAdapter adapter{scripted_options(std::make_shared<ScriptedFactory>(Behaviour{}))};

    const auto result = coro::run(adapter.resolve(interface_config(domain::RecordKind::A, loopback)));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(every_address_is(*result, domain::AddressFamily::IPV4));
    EXPECT_EQ(result->size(), keep_family(*known, domain::AddressFamily::IPV4).size());
}

TEST(IpSourceAdapter, RecordKindAaaaSelectsOnlyIpv6Addresses) {
    const std::string loopback = loopback_interface();
    const auto known = ipsource::get_addresses(loopback);
    if (loopback.empty() || !known.has_value()) {
        GTEST_SKIP() << "no interface owns 127.0.0.1";
    }
    ipsource::IpSourceAdapter adapter{scripted_options(std::make_shared<ScriptedFactory>(Behaviour{}))};

    const auto result = coro::run(adapter.resolve(interface_config(domain::RecordKind::AAAA, loopback)));

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(every_address_is(*result, domain::AddressFamily::IPV6));
    EXPECT_EQ(result->size(), keep_family(*known, domain::AddressFamily::IPV6).size());
}

TEST(IpSourceAdapter, RecordKindTxtLeavesTheFamilyUnspecified) {
    const std::string loopback = loopback_interface();
    const auto known = ipsource::get_addresses(loopback);
    if (loopback.empty() || !known.has_value()) {
        GTEST_SKIP() << "no interface owns 127.0.0.1";
    }
    ipsource::IpSourceAdapter adapter{scripted_options(std::make_shared<ScriptedFactory>(Behaviour{}))};

    const auto result = coro::run(adapter.resolve(interface_config(domain::RecordKind::TXT, loopback)));

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), known->size());
}

TEST(IpSourceAdapter, HttpSourceBuildsItsOwnSourceFromTheSharedPolicy) {
    auto factory = std::make_shared<ScriptedFactory>(Behaviour{.incoming = http_response("198.51.100.9")});
    ipsource::IpSourceAdapter adapter{scripted_options(factory)};
    domain::SubdomainConfig config;
    config.type = domain::RecordKind::A;
    config.ip_source = domain::IpSource::HTTP;
    config.ip_source_param = std::string{SERVER_URL};
    config.interface = "yaddnsc-bind0";

    const auto result = coro::run(adapter.resolve(config));

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->size(), 1U);
    EXPECT_EQ(result->front().to_string(), "198.51.100.9");
    // The A record pinned the family to IPv4 and the config interface pinned
    // the outbound device; both reached the transport.
    EXPECT_EQ(factory->tcp_calls, 1);
    EXPECT_EQ(factory->bound_interface, std::optional<std::string>{"yaddnsc-bind0"});
}

TEST(IpSourceAdapter, InterfaceFailurePassesThroughUnchanged) {
    if (ipsource::get_addresses(std::string{ABSENT_INTERFACE}).has_value()) {
        GTEST_SKIP() << "the probe interface name is in use";
    }
    ipsource::IpSourceAdapter adapter{scripted_options(std::make_shared<ScriptedFactory>(Behaviour{}))};

    const auto result =
        coro::run(adapter.resolve(interface_config(domain::RecordKind::A, std::string{ABSENT_INTERFACE})));

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, IpSourceError::Code::UNAVAILABLE);
    EXPECT_NE(result.error().message.find(ABSENT_INTERFACE), std::string::npos);
}

}  // namespace