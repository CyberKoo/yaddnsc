//
// UpdateWorkflow unit tests — exercises the single-task update workflow
// against the application ports: MockDnsResolverPort, MockIpSourcePort,
// MockDriverGateway and a NullLogger.
//
// Behaviour locked here (legacy Updater semantics):
//   - IP unchanged            → driver not invoked        (SkipUnchanged)
//   - IP changed / DNS fails  → update attempted          (UpdateChanged)
//   - force_update            → DNS comparison skipped    (UpdateForced)
//   - no candidate address    → update skipped            (SKIPPED_NO_ADDRESS)
//   - driver failure          → UpdateError::DRIVER_FAILED, logged, never thrown
// The AAAA link-local/ULA filtering rules live in domain::select_address and
// are covered exhaustively by domain/address_policy_test.cpp; here only the
// workflow wiring (subdomain flags → policy) is checked.
//

#include "application/update_workflow.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <expected>
#include <glaze/glaze.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "application/ports/driver_gateway.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/error/error.h"
#include "domain/fqdn.h"
#include "domain/network/inet_address.h"
#include "domain/update/update_decision.h"
#include "domain/update/update_task.h"
#include "fixtures/sample_config.h"
#include "infrastructure/config/config.h"
#include "infrastructure/config/parser.hpp"  // IWYU pragma: keep — registers glz::meta specializations
#include "infrastructure/config/normalizer.h"
#include "mocks/mock_ports.h"
#include "mocks/null_logger.h"
#include "support/util/cancellation_token.hpp"

namespace {

using ::testing::_;
using ::testing::Return;

// ── Helpers ─────────────────────────────────────────────────────────────────

[[nodiscard]] std::shared_ptr<const domain::RuntimeConfig> parse_cfg(std::string_view json) {
    auto cfg = Config::AppConfig{};
    const auto ec = glz::read<glz::opts{.error_on_missing_keys = false}>(cfg, json);
    EXPECT_EQ(ec, glz::error_code::none) << glz::format_error(ec, json);
    return std::make_shared<const domain::RuntimeConfig>(Config::normalize(cfg));
}

// Parse a config, apply a mutation to the raw DTO before normalising, and
// return the shared runtime config. Used by tests that override a subdomain
// setting (the shared config is const once handed out).
template<typename Mutator>
[[nodiscard]] std::shared_ptr<const domain::RuntimeConfig> parse_cfg_mut(std::string_view json, Mutator mut) {
    auto cfg = Config::AppConfig{};
    const auto ec = glz::read<glz::opts{.error_on_missing_keys = false}>(cfg, json);
    EXPECT_EQ(ec, glz::error_code::none) << glz::format_error(ec, json);
    mut(cfg);
    return std::make_shared<const domain::RuntimeConfig>(Config::normalize(cfg));
}

// Build a single-subdomain task from the shared fixture config.
[[nodiscard]] domain::UpdateTask make_task(const std::shared_ptr<const domain::RuntimeConfig>& cfg,
                                           std::size_t domain_idx = 0,
                                           std::size_t sub_idx = 0) {
    const auto& domain = cfg->domains[domain_idx];
    const auto& sub = domain.subdomains[sub_idx];
    return domain::UpdateTask{
        .config = cfg,
        .domain_index = domain_idx,
        .subdomain_index = sub_idx,
        .fqdn = domain::make_fqdn(domain.name, sub.name),
        .force_update = false,
    };
}

[[nodiscard]] std::vector<InetAddress> one_v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    return {InetAddress{Inet4Address::from_bytes({a, b, c, d})}};
}

[[nodiscard]] InetAddress link_local_v6() {
    return InetAddress{Inet6Address::from_bytes({0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})};
}

struct Ports {
    MockDnsResolverPort dns;
    MockIpSourcePort ip_source;
    MockDriverGateway gateway;
    NullLogger logger;
};

}  // namespace

// ── IP unchanged → driver not invoked ─────────────────────────────────────────

TEST(UpdateWorkflow, SkipsUpdateWhenIpUnchanged) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(one_v4(192, 0, 2, 1)));
    EXPECT_CALL(ports.dns, resolve(task.fqdn, RecordKind::A, _)).WillOnce(Return(std::vector<std::string>{"192.0.2.1"}));
    EXPECT_CALL(ports.gateway, update(_, _, _)).Times(0);

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::SkipUnchanged);
}

// ── IP changed → driver invoked with the mapped command ──────────────────────

TEST(UpdateWorkflow, UpdatesWhenIpChanged) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(one_v4(198, 51, 100, 1)));
    EXPECT_CALL(ports.dns, resolve(task.fqdn, RecordKind::A, _)).WillOnce(Return(std::vector<std::string>{"192.0.2.1"}));
    EXPECT_CALL(ports.gateway, update("cloudflare", _, _))
        .WillOnce(
            [&task](std::string_view,
                    const DriverUpdateCommand& cmd,
                    const Utils::CancellationToken&) -> std::expected<void, domain::DriverError> {
                EXPECT_EQ(cmd.ip_addr, "198.51.100.1");
                EXPECT_EQ(cmd.rd_type, "A");
                EXPECT_EQ(cmd.domain, "example.com");
                EXPECT_EQ(cmd.subdomain, "@");
                EXPECT_EQ(cmd.fqdn, task.fqdn);
                EXPECT_FALSE(cmd.driver_param.empty());
                return {};
            });

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UpdateChanged);
}

// ── force_update → DNS comparison skipped ─────────────────────────────────────

TEST(UpdateWorkflow, ForceUpdateSkipsDnsComparison) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);
    task.force_update = true;

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(one_v4(192, 0, 2, 1)));
    // Even though the IP would equal the DNS record, force_update must still
    // update — and must not even ask the resolver.
    EXPECT_CALL(ports.dns, resolve(_, _, _)).Times(0);
    EXPECT_CALL(ports.gateway, update(_, _, _)).WillOnce(Return(std::expected<void, domain::DriverError>{}));

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UpdateForced);
}

// ── empty IP source → update skipped ──────────────────────────────────────────

TEST(UpdateWorkflow, SkipsWhenIpSourceReturnsEmpty) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(std::vector<InetAddress>{}));
    EXPECT_CALL(ports.dns, resolve(_, _, _)).Times(0);
    EXPECT_CALL(ports.gateway, update(_, _, _)).Times(0);

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::SKIPPED_NO_ADDRESS);
}

// ── DNS lookup failure → still attempts update ────────────────────────────────

TEST(UpdateWorkflow, UpdatesWhenDnsLookupFails) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(one_v4(198, 51, 100, 1)));
    EXPECT_CALL(ports.dns, resolve(_, _, _))
        .WillOnce(Return(std::unexpected(DnsErrorInfo{DnsError::NX_DOMAIN, "domain does not exist"})));
    EXPECT_CALL(ports.gateway, update(_, _, _)).WillOnce(Return(std::expected<void, domain::DriverError>{}));

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UpdateChanged);
}

// ── DNS success with zero records → still updates ────────────────────────────

TEST(UpdateWorkflow, UpdatesWhenDnsReturnsEmptyRecordList) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(one_v4(198, 51, 100, 1)));
    EXPECT_CALL(ports.dns, resolve(_, _, _)).WillOnce(Return(std::vector<std::string>{}));
    EXPECT_CALL(ports.gateway, update(_, _, _)).WillOnce(Return(std::expected<void, domain::DriverError>{}));

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UpdateChanged);
}

// ── driver reports failure → DRIVER_FAILED error value, no throw ─────────────

TEST(UpdateWorkflow, DriverFailureReturnsDriverFailed) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(one_v4(198, 51, 100, 1)));
    EXPECT_CALL(ports.dns, resolve(_, _, _)).WillOnce(Return(std::vector<std::string>{"192.0.2.1"}));
    EXPECT_CALL(ports.gateway, update(_, _, _))
        .WillOnce(Return(
            std::unexpected(domain::DriverError{domain::DriverError::Code::UPDATE_FAILED, "upstream rejected"})));

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::DRIVER_FAILED);
    EXPECT_EQ(outcome.error().message, "upstream rejected");
    EXPECT_EQ(outcome.error().retry_after_seconds, 0);
}

// ── rate-limited driver → retry_after carried (but never rescheduled here) ───

TEST(UpdateWorkflow, RateLimitedCarriesRetryAfterIntoUpdateError) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(one_v4(198, 51, 100, 1)));
    EXPECT_CALL(ports.dns, resolve(_, _, _)).WillOnce(Return(std::vector<std::string>{"192.0.2.1"}));
    EXPECT_CALL(ports.gateway, update(_, _, _))
        .WillOnce(
            Return(std::unexpected(domain::DriverError{domain::DriverError::Code::RATE_LIMITED, "slow down", 120})));

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::DRIVER_FAILED);
    // The executor forwards any positive retry-after—whether supplied by a
    // provider rate limit or a transport failure—to the scheduler.
    EXPECT_EQ(outcome.error().retry_after_seconds, 120);
}

// ── driver not found / driver exception → DRIVER_FAILED, no throw ─────────────

TEST(UpdateWorkflow, DriverNotFoundReturnsDriverFailed) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(one_v4(198, 51, 100, 1)));
    EXPECT_CALL(ports.dns, resolve(_, _, _)).WillOnce(Return(std::vector<std::string>{"192.0.2.1"}));
    EXPECT_CALL(ports.gateway, update(_, _, _))
        .WillOnce(
            Return(std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND, "driver not loaded"})));

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::DRIVER_FAILED);
}

TEST(UpdateWorkflow, DriverUnknownErrorReturnsDriverFailed) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(one_v4(198, 51, 100, 1)));
    EXPECT_CALL(ports.dns, resolve(_, _, _)).WillOnce(Return(std::vector<std::string>{"192.0.2.1"}));
    EXPECT_CALL(ports.gateway, update(_, _, _))
        .WillOnce(Return(std::unexpected(
            domain::DriverError{domain::DriverError::Code::UNKNOWN, "Driver configuration parse error: ..."})));

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::DRIVER_FAILED);
}

// ── AAAA policy wiring: link-local filtered unless allowed ────────────────────

TEST(UpdateWorkflow, FiltersLinkLocalForAaaaWhenNotAllowed) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    // The "www" subdomain is type AAAA, interface source, allow_local_link=false.
    auto task = make_task(cfg, 0, 1);

    Ports ports;
    // Only a link-local candidate is available; it must be filtered out.
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(std::vector<InetAddress>{link_local_v6()}));
    EXPECT_CALL(ports.gateway, update(_, _, _)).Times(0);

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::SKIPPED_NO_ADDRESS);
}

TEST(UpdateWorkflow, KeepsLinkLocalForAaaaWhenAllowed) {
    auto task = make_task(parse_cfg_mut(Fixtures::FULL_CONFIG,
                                        [](Config::AppConfig& cfg) {
                                            cfg.domains[0].subdomains[1].allow_local_link = true;  // override
                                        }),
                          0, 1);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(std::vector<InetAddress>{link_local_v6()}));
    EXPECT_CALL(ports.dns, resolve(_, _, _)).WillOnce(Return(std::vector<std::string>{"2001:db8::1"}));
    EXPECT_CALL(ports.gateway, update(_, _, _))
        .WillOnce([](std::string_view,
                     const DriverUpdateCommand& cmd,
                     const Utils::CancellationToken&) -> std::expected<void, domain::DriverError> {
            EXPECT_EQ(cmd.ip_addr, "fe80::1");
            EXPECT_EQ(cmd.rd_type, "AAAA");
            return {};
        });

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UpdateChanged);
}

// ── IP source failure arrives as an error value → update skipped ─────────────
// (The throwing-implementation → error-value conversion is the adapter's
// contract, covered by ip_source/adapter_test.cpp.)

TEST(UpdateWorkflow, SkipsWhenIpSourceFails) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _))
        .WillOnce(Return(
            std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE, "interface not found"})));
    EXPECT_CALL(ports.dns, resolve(_, _, _)).Times(0);
    EXPECT_CALL(ports.gateway, update(_, _, _)).Times(0);

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::SKIPPED_NO_ADDRESS);
    EXPECT_EQ(outcome.error().message, "interface not found");
}

// ── Multiple IP candidates → first candidate used ────────────────────────────

TEST(UpdateWorkflow, MultipleIpCandidates_PicksFirst) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _))
        .WillOnce(Return(std::vector<InetAddress>{
            InetAddress{Inet4Address::from_bytes({10, 0, 0, 1})},
            InetAddress{Inet4Address::from_bytes({198, 51, 100, 1})},
        }));
    EXPECT_CALL(ports.dns, resolve(_, _, _)).WillOnce(Return(std::vector<std::string>{"192.0.2.1"}));
    EXPECT_CALL(ports.gateway, update(_, _, _))
        .WillOnce([](std::string_view,
                     const DriverUpdateCommand& cmd,
                     const Utils::CancellationToken&) -> std::expected<void, domain::DriverError> {
            EXPECT_EQ(cmd.ip_addr, "10.0.0.1");
            return {};
        });

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->decision, domain::UpdateDecision::UpdateChanged);
}

TEST(UpdateWorkflow, CancelledDnsLookupDoesNotInvokeDriver) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(one_v4(198, 51, 100, 1)));
    EXPECT_CALL(ports.dns, resolve(task.fqdn, RecordKind::A, _))
        .WillOnce(Return(std::unexpected(DnsErrorInfo{DnsError::CANCELLED, "cancelled"})));
    EXPECT_CALL(ports.gateway, update(_, _, _)).Times(0);

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::CANCELLED);
}

TEST(UpdateWorkflow, CancelledIpSourceDoesNotInvokeDnsOrDriver) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _))
        .WillOnce(Return(std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::CANCELLED, "cancelled"})));
    EXPECT_CALL(ports.dns, resolve(_, _, _)).Times(0);
    EXPECT_CALL(ports.gateway, update(_, _, _)).Times(0);

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::CANCELLED);
}

TEST(UpdateWorkflow, CancellationBeforeDriverDoesNotInvokeDriver) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);
    task.force_update = true;

    Utils::CancellationSource source;
    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce([&source](const domain::SubdomainConfig&,
                                                                   const Utils::CancellationToken&) {
        source.trigger();
        return std::expected<std::vector<InetAddress>, domain::IpSourceError>{one_v4(198, 51, 100, 1)};
    });
    EXPECT_CALL(ports.dns, resolve(_, _, _)).Times(0);
    EXPECT_CALL(ports.gateway, update(_, _, _)).Times(0);

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, source.token());

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::CANCELLED);
}

TEST(UpdateWorkflow, DriverCancellationReturnsCancelled) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce(Return(one_v4(198, 51, 100, 1)));
    EXPECT_CALL(ports.dns, resolve(_, _, _)).WillOnce(Return(std::vector<std::string>{"192.0.2.1"}));
    EXPECT_CALL(ports.gateway, update(_, _, _))
        .WillOnce(Return(std::unexpected(domain::DriverError{domain::DriverError::Code::CANCELLED, "cancelled"})));

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::CANCELLED);
    EXPECT_EQ(outcome.error().message, "cancelled");
}

TEST(UpdateWorkflow, StandardExceptionIsTranslatedToUnknown) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce([](const domain::SubdomainConfig&,
                                                            const Utils::CancellationToken&) {
        throw std::runtime_error("unexpected failure");
        return std::expected<std::vector<InetAddress>, domain::IpSourceError>{};
    });

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::UNKNOWN);
    EXPECT_EQ(outcome.error().message, "unexpected failure");
}

TEST(UpdateWorkflow, NonStandardExceptionIsTranslatedToUnknown) {
    auto cfg = parse_cfg(Fixtures::FULL_CONFIG);
    auto task = make_task(cfg);

    Ports ports;
    EXPECT_CALL(ports.ip_source, resolve(_, _)).WillOnce([](const domain::SubdomainConfig&,
                                                            const Utils::CancellationToken&) {
        throw 42;
        return std::expected<std::vector<InetAddress>, domain::IpSourceError>{};
    });

    const UpdateWorkflow workflow(ports.dns, ports.ip_source, ports.gateway, ports.logger);
    const auto outcome = workflow.run(task, {});

    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code, domain::UpdateError::Code::UNKNOWN);
    EXPECT_EQ(outcome.error().message, "Unknown non-standard exception");
}
