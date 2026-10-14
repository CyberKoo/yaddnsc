// Replace RAND_bytes only in this executable: no production fault hooks and
// no process-global OpenSSL configuration changes in the normal test suites.
#include <chrono>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <openssl/rand.h>

#include "coro/coro.h"
#include "domain/config/dns_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/bootstrap/bootstrap.h"
#include "infrastructure/dns/dispatcher.h"
#include "infrastructure/dns/policy.h"
#include "infrastructure/dns/resolver/classic.h"
#include "infrastructure/dns/resolver/doh.h"
#include "infrastructure/dns/resolver/dot.h"
#include "infrastructure/dns/resolver/resolver.h"
#include "infrastructure/dns/types.h"
#include "infrastructure/dns/wire/builder.h"
#include "infrastructure/http/types.h"
#include "infrastructure/network/transport/stream.h"
#include "support/util/random.hpp"

namespace {
int rand_calls = 0;
}

extern "C" int RAND_bytes(unsigned char*, int) {
    ++rand_calls;
    return 0;
}

namespace {

class RejectingFactory final : public net::StreamFactory {
public:
    std::unique_ptr<net::Stream> create_tls(domain::InetAddress, std::uint16_t, const net::ConnectOptions&,
                                            const net::TlsOptions&, std::shared_ptr<const net::TlsContext>) override {
        ++calls_;
        throw std::logic_error("Entropy failure must precede connecting");
    }

    std::unique_ptr<net::Stream> create_tcp(domain::InetAddress, std::uint16_t, const net::ConnectOptions&) override {
        ++calls_;
        throw std::logic_error("Entropy failure must precede connecting");
    }

    int calls_ = 0;
};

TEST(RandomCryptoTest, SourceFailure_ReturnsErrorWithoutPrngFallback) {
    const int before = rand_calls;
    const auto value = Utils::Random::crypto_u16();
    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(rand_calls, before + 1);
    EXPECT_STREQ(value.error().what(), "Cryptographic random source unavailable");
}

TEST(QueryBuilderTest, SourceFailure_AbortsRandomIdConstruction) {
    EXPECT_THROW(dns::QueryBuilder{}, Utils::Random::EntropyError);
}

TEST(QueryBuilderTest, ExplicitId_DoesNotAcquireEntropy) {
    const int before = rand_calls;
    const auto query = dns::QueryBuilder{0}.rd(false).add_question("printer.local", dns::RecordType::A).build();
    ASSERT_GE(query.size(), 12U);
    EXPECT_EQ(query[0], 0);
    EXPECT_EQ(query[1], 0);
    EXPECT_EQ(rand_calls, before);
}

TEST(DnsResolvers, SourceFailure_IsOperationalAndPrecedesIo) {
    const auto address = domain::InetAddress::parse("127.0.0.1").value();
    auto factory = std::make_shared<RejectingFactory>();
    http::Options http_options;
    http_options.factory = factory;
    dns::EndpointOptions dot_options;
    dot_options.factory = factory;
    std::vector<std::unique_ptr<dns::Resolver>> resolvers;
    resolvers.push_back(std::make_unique<dns::ClassicResolver>(address, 9));
    resolvers.push_back(std::make_unique<dns::DohResolver>("https://127.0.0.1/dns-query", std::move(http_options)));
    resolvers.push_back(std::make_unique<dns::DotResolver>("127.0.0.1", 853, std::move(dot_options)));
    for (const auto& resolver : resolvers) {
        SCOPED_TRACE(resolver->name());
        coro::ManualClock clock;
        coro::Loop loop{clock};
        const auto result = coro::run(loop, resolver->query("x.test", domain::RecordKind::A));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, domain::DnsError::CONNECTION);
        EXPECT_EQ(result.error().message, "Cryptographic random source unavailable");
    }
    EXPECT_EQ(factory->calls_, 0);
}

TEST(Dispatcher, SourceFailure_BackendsReturnConnectionAndRetry) {
    for (const auto strategy : {dns::Strategy::FALLBACK, dns::Strategy::SHUFFLE, dns::Strategy::CONCURRENT}) {
        SCOPED_TRACE(static_cast<int>(strategy));
        for (const int backend_count : {1, 3}) {
            SCOPED_TRACE(backend_count);
            const auto address = domain::InetAddress::parse("127.0.0.1").value();
            auto factory = std::make_shared<RejectingFactory>();
            std::vector<std::unique_ptr<dns::Resolver>> resolvers;
            resolvers.push_back(std::make_unique<dns::ClassicResolver>(address, 9));
            if (backend_count == 3) {
                http::Options http_options;
                http_options.factory = factory;
                dns::EndpointOptions dot_options;
                dot_options.factory = factory;
                resolvers.push_back(
                    std::make_unique<dns::DohResolver>("https://127.0.0.1/dns-query", std::move(http_options)));
                resolvers.push_back(std::make_unique<dns::DotResolver>("127.0.0.1", 853, std::move(dot_options)));
            }
            dns::Policy policy;
            policy.max_retries = 1;
            policy.retry_backoff = std::chrono::milliseconds{1};
            dns::Dispatcher dispatcher{std::move(resolvers), strategy, policy};
            coro::ManualClock clock;
            coro::Loop loop{clock};
            const int before = rand_calls;
            const auto result = coro::run(loop, dispatcher.resolve("x.test", domain::RecordKind::A));
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().code, domain::DnsError::CONNECTION);
            EXPECT_EQ(result.error().message, "Cryptographic random source unavailable");
            // Preflight acquires no entropy. Each backend gets one initial
            // attempt and one retry, without reaching the socket/HTTP layer.
            EXPECT_EQ(rand_calls - before, backend_count * 2);
            EXPECT_EQ(factory->calls_, 0);
            EXPECT_EQ(clock.now(), coro::TimePoint{std::chrono::milliseconds{1}});
        }
    }
}

TEST(Dispatcher, InvalidQuery_RejectsBeforeEntropyAcquisition) {
    const std::vector<std::string> invalid_hosts{"bad..test", std::string(64, 'a') + ".test",
                                                 std::string{"x\0.test", 7}};
    for (const auto& host : invalid_hosts) {
        SCOPED_TRACE(host);
        std::vector<std::unique_ptr<dns::Resolver>> resolvers;
        resolvers.push_back(std::make_unique<dns::ClassicResolver>(domain::InetAddress::parse("127.0.0.1").value(), 9));
        dns::Dispatcher dispatcher{std::move(resolvers)};
        coro::ManualClock clock;
        coro::Loop loop{clock};
        const int before = rand_calls;
        const auto result = coro::run(loop, dispatcher.resolve(host, domain::RecordKind::A));
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, domain::DnsError::INVALID_QUERY);
        EXPECT_EQ(rand_calls, before);
    }
}

TEST(BootstrapResolve, SourceFailure_IsOperationalNotInvalidQuery) {
    coro::ManualClock clock;
    coro::Loop loop{clock};
    const std::vector<domain::DnsServer> servers{{"127.0.0.1", 9}};
    const auto result = coro::run(loop, dns::bootstrap_resolve("x.test", domain::AddressFamily::IPV4, servers));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DnsError::CONNECTION);
}

}  // namespace
