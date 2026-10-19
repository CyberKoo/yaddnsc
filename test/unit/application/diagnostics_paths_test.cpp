//
// Diagnostics path tests — list_drivers, list_interfaces, dns_resolve and
// validate_driver_configs.
//
// diagnostics_test.cpp drives the happy paths and the wording of the failure
// messages; this file drives the *shape* of the input instead: an empty
// enumeration, a multi-row enumeration, a port that throws, an unchecked
// driver named twice, and every record kind the parser accepts. Each of those
// takes a different arm in the plain (non-coroutine) loops of
// src/application/diagnostics.cpp and src/application/config_validation.cpp.
//

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <coroutine>

#include <expected>
#include <gtest/gtest.h>

#include "application/config_validation.h"
#include "application/diagnostics.h"
#include "application/ports/driver_catalog.h"
#include "application/ports/gateway.h"
#include "application/ports/network_interfaces.h"
#include "application/ports/resolver.h"
#include "coro/coro.h"
#include "domain/config/runtime_config.h"
#include "domain/dns/record_kind.h"
#include "domain/error/dns_error_info.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "domain/update/driver_update_command.h"

namespace {

using namespace std::chrono_literals;
using DescribeResult = std::expected<app::DriverDescription, domain::DriverError>;
using LookupResult = std::expected<std::vector<std::string>, domain::DnsErrorInfo>;
using ValidatedCalls = std::vector<std::pair<std::string, std::string>>;

/// Catalog double whose enumeration length and per-driver verdict the test
/// sets; `loaded_throws` / `describe_throws` model the port's documented
/// "allocation failures propagate" contract.
class ScriptedCatalog final : public app::DriverCatalogPort {
public:
    std::vector<std::string> names;
    /// Names describe() answers with DriverError::NOT_FOUND.
    std::vector<std::string> missing;
    bool loaded_throws{false};
    bool describe_throws{false};
    /// Names passed to describe(), in order (the port method is const).
    mutable std::vector<std::string> described;

    [[nodiscard]] std::vector<std::string> loaded_drivers() const override {
        if (loaded_throws) {
            throw std::bad_alloc{};
        }
        return names;
    }

    [[nodiscard]] DescribeResult describe(std::string_view name) const override {
        if (describe_throws) {
            throw std::runtime_error("describe failed");
        }
        described.emplace_back(name);
        for (const auto& absent : missing) {
            if (absent == name) {
                return std::unexpected(domain::DriverError{domain::DriverError::Code::NOT_FOUND, {}});
            }
        }
        return described_plugin(std::string(name));
    }

    [[nodiscard]] static DescribeResult described_plugin(std::string name) {
        return app::DriverDescription{
            .name = std::move(name),
            .version = "1.0",
            .author = "tests",
            .description = "scripted",
            .supported_records = {domain::RecordKind::A},
            .supports_validate = true,
        };
    }
};

/// Catalog that accepts its first name and throws on the second: the defect
/// lands mid-enumeration, after one row already exists.
class HalfThrowingCatalog final : public app::DriverCatalogPort {
public:
    [[nodiscard]] std::vector<std::string> loaded_drivers() const override { return {"alpha", "beta"}; }

    [[nodiscard]] DescribeResult describe(std::string_view name) const override {
        if (name == "beta") {
            throw std::runtime_error("second describe failed");
        }
        return ScriptedCatalog::described_plugin(std::string(name));
    }
};

class ScriptedInterfaces final : public app::NetworkInterfacesPort {
public:
    std::vector<app::InterfaceInfo> rows;
    bool list_throws{false};

    [[nodiscard]] std::vector<std::string> names() const override {
        std::vector<std::string> out;
        out.reserve(rows.size());
        for (const auto& row : rows) {
            out.push_back(row.name);
        }
        return out;
    }

    [[nodiscard]] std::optional<std::vector<domain::InetAddress>> addresses(const std::string&) const override {
        return std::nullopt;
    }

    [[nodiscard]] std::vector<app::InterfaceInfo> list() const override {
        if (list_throws) {
            throw std::runtime_error("enumeration failed");
        }
        return rows;
    }
};

class ScriptedResolver final : public app::ResolverPort {
public:
    std::vector<domain::RecordKind> asked;
    std::vector<std::string> records{"192.0.2.1"};

    [[nodiscard]] coro::Task<LookupResult> resolve(std::string, domain::RecordKind kind) override {
        asked.push_back(kind);
        co_return records;
    }
};

class ScriptedGateway final : public app::GatewayPort {
public:
    /// Drivers without a validation entry; every other driver accepts.
    std::vector<std::string> unsupported;
    ValidatedCalls validated;

    [[nodiscard]] bool supports_validate(std::string_view name) const override {
        return std::find(unsupported.begin(), unsupported.end(), name) == unsupported.end();
    }

    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> validate_config(std::string driver,
                                                                                       std::string params) override {
        validated.emplace_back(std::move(driver), std::move(params));
        co_return std::expected<void, domain::DriverError>{};
    }

    [[nodiscard]] coro::Task<std::expected<void, domain::DriverError>> update(std::string,
                                                                              domain::DriverUpdateCommand) override {
        co_return std::expected<void, domain::DriverError>{};
    }
};

[[nodiscard]] domain::SubdomainConfig make_subdomain(std::string name, std::string params) {
    return domain::SubdomainConfig{.name = std::move(name), .driver_params = std::move(params)};
}

[[nodiscard]] domain::DomainConfig make_domain(std::string name, std::string driver,
                                               std::vector<domain::SubdomainConfig> subdomains) {
    return domain::DomainConfig{
        .name = std::move(name),
        .driver = std::move(driver),
        .subdomains = std::move(subdomains),
    };
}

}  // namespace

// ---------------------------------------------------------------------------
// list_drivers: enumeration shape and port failure
// ---------------------------------------------------------------------------

TEST(DiagnosticsPaths, ListDrivers_EmptyCatalog_ReturnsNoRows) {
    // The zero-iteration arm of the listing loop: nothing is described, and
    // the caller gets an empty (not null) vector back.
    ScriptedCatalog catalog;

    const auto items = app::list_drivers(catalog);

    EXPECT_TRUE(items.empty());
    EXPECT_TRUE(catalog.described.empty());
}

TEST(DiagnosticsPaths, ListDrivers_ManyDrivers_DescribesEachOnce) {
    ScriptedCatalog catalog;
    catalog.names = {"alpha", "beta", "gamma"};

    const auto items = app::list_drivers(catalog);

    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(catalog.described, (std::vector<std::string>{"alpha", "beta", "gamma"}));
    for (const auto& item : items) {
        EXPECT_TRUE(item.detail.has_value());
        EXPECT_EQ(item.detail->name, item.name);
    }
}

TEST(DiagnosticsPaths, ListDrivers_MixedVerdicts_KeepsBothOutcomes) {
    // describe() answers per driver: a missing plugin is a row carrying an
    // error, not a dropped row.
    ScriptedCatalog catalog;
    catalog.names = {"alpha", "ghost", "gamma"};
    catalog.missing = {"ghost"};

    const auto items = app::list_drivers(catalog);

    ASSERT_EQ(items.size(), 3u);
    EXPECT_TRUE(items[0].detail.has_value());
    ASSERT_FALSE(items[1].detail.has_value());
    EXPECT_EQ(items[1].detail.error().code, domain::DriverError::Code::NOT_FOUND);
    EXPECT_TRUE(items[2].detail.has_value());
}

TEST(DiagnosticsPaths, ListDrivers_DescribeThrows_Propagates) {
    // A descriptor query that throws is a defect, not a missing driver.
    ScriptedCatalog catalog;
    catalog.names = {"alpha"};
    catalog.describe_throws = true;

    EXPECT_THROW({ [[maybe_unused]] const auto items = app::list_drivers(catalog); }, std::runtime_error);
}

TEST(DiagnosticsPaths, ListDrivers_DescribeThrowsMidway_Propagates) {
    // The defect lands on the second name, after one row already exists: the
    // finished rows must not downgrade the throw into a short listing.
    HalfThrowingCatalog catalog;

    EXPECT_THROW({ [[maybe_unused]] const auto items = app::list_drivers(catalog); }, std::runtime_error);
}

TEST(DiagnosticsPaths, ListDrivers_LoadedDriversThrows_Propagates) {
    // The enumeration itself failed: there is no row to report, so the defect
    // escapes instead of being swallowed into an empty listing.
    ScriptedCatalog catalog;
    catalog.loaded_throws = true;

    EXPECT_THROW({ [[maybe_unused]] const auto items = app::list_drivers(catalog); }, std::bad_alloc);
}

// ---------------------------------------------------------------------------
// list_interfaces: enumeration shape and port failure
// ---------------------------------------------------------------------------

TEST(DiagnosticsPaths, ListInterfaces_EmptyEnumeration_ReturnsNoRows) {
    ScriptedInterfaces interfaces;

    const auto items = app::list_interfaces(interfaces);

    EXPECT_TRUE(items.empty());
}

TEST(DiagnosticsPaths, ListInterfaces_ManyRows_KeepsEveryRow) {
    ScriptedInterfaces interfaces;
    const auto loopback = domain::InetAddress::parse("127.0.0.1");
    ASSERT_TRUE(loopback.has_value());
    interfaces.rows = {
        app::InterfaceInfo{.name = "lo", .addresses = {*loopback}},
        app::InterfaceInfo{.name = "eth0", .addresses = {}},
        app::InterfaceInfo{.name = "eth1", .addresses = {*loopback}},
        app::InterfaceInfo{.name = "eth2", .addresses = {}},
    };

    const auto items = app::list_interfaces(interfaces);

    ASSERT_EQ(items.size(), 4u);
    for (std::size_t i = 0; i < items.size(); ++i) {
        EXPECT_EQ(items[i].name, interfaces.rows[i].name);
    }
}

TEST(DiagnosticsPaths, ListInterfaces_ListThrows_Propagates) {
    ScriptedInterfaces interfaces;
    interfaces.list_throws = true;

    EXPECT_THROW({ [[maybe_unused]] const auto items = app::list_interfaces(interfaces); }, std::runtime_error);
}

// ---------------------------------------------------------------------------
// dns_resolve: every record kind the parser accepts
// ---------------------------------------------------------------------------

TEST(DiagnosticsPaths, DnsResolve_EveryKnownKind_ReachesTheResolver) {
    const std::vector<std::pair<std::string, domain::RecordKind>> kinds{
        {"A", domain::RecordKind::A},
        {"AAAA", domain::RecordKind::AAAA},
        {"TXT", domain::RecordKind::TXT},
    };
    for (const auto& [text, kind] : kinds) {
        ScriptedResolver resolver;
        coro::Loop loop;

        const auto outcome = coro::run(loop, app::dns_resolve(resolver, "example.com", text));

        ASSERT_TRUE(outcome.lookup.has_value()) << text;
        EXPECT_TRUE(outcome.lookup->has_value()) << text;
        ASSERT_EQ(resolver.asked.size(), 1u) << text;
        EXPECT_EQ(resolver.asked.front(), kind) << text;
    }
}

// ---------------------------------------------------------------------------
// validate_driver_configs: sweep shape
// ---------------------------------------------------------------------------

TEST(DiagnosticsPaths, ValidateDriverConfigs_EmptyConfig_PassesInBothModes) {
    // The zero-domain arm of the sweep: nothing to check is a pass, and the
    // REQUIRE_VALIDATION policy must not turn "no records" into a rejection.
    const domain::RuntimeConfig empty;
    for (const auto mode :
         {app::ConfigValidationMode::WARN_AND_CONTINUE, app::ConfigValidationMode::REQUIRE_VALIDATION}) {
        ScriptedGateway gateway;
        coro::Loop loop;

        const auto outcome = coro::run(loop, app::validate_driver_configs(gateway, empty, mode, 5s));

        EXPECT_TRUE(outcome.rejection.empty());
        EXPECT_TRUE(outcome.unchecked_drivers.empty());
        EXPECT_TRUE(gateway.validated.empty());
    }
}

TEST(DiagnosticsPaths, ValidateDriverConfigs_RequireValidation_AcceptsCheckedDriver) {
    // The REQUIRE_VALIDATION arm that does not reject: a driver exporting the
    // entry and accepting its parameters passes the `config test` sweep.
    ScriptedGateway gateway;
    const domain::RuntimeConfig config{
        .domains = {make_domain("example.com", "fake", {make_subdomain("www", "{}")})},
    };
    coro::Loop loop;

    const auto outcome = coro::run(
        loop, app::validate_driver_configs(gateway, config, app::ConfigValidationMode::REQUIRE_VALIDATION, 5s));

    EXPECT_TRUE(outcome.rejection.empty());
    EXPECT_TRUE(outcome.unchecked_drivers.empty());
    EXPECT_EQ(gateway.validated, (ValidatedCalls{{"fake", "{}"}}));
}

TEST(DiagnosticsPaths, ValidateDriverConfigs_WarnAndContinue_NamesUncheckedDriverOnce) {
    // The same legacy driver behind several records and two domains is named
    // exactly once: the warning list is a set of drivers, not of records.
    ScriptedGateway gateway;
    gateway.unsupported = {"legacy"};
    const domain::RuntimeConfig config{
        .domains =
            {
                make_domain("a.example.com", "legacy", {make_subdomain("@", "{}"), make_subdomain("www", "{}")}),
                make_domain("b.example.com", "legacy", {make_subdomain("@", "{}")}),
            },
    };
    coro::Loop loop;

    const auto outcome = coro::run(
        loop, app::validate_driver_configs(gateway, config, app::ConfigValidationMode::WARN_AND_CONTINUE, 5s));

    EXPECT_TRUE(outcome.rejection.empty());
    EXPECT_EQ(outcome.unchecked_drivers, std::vector<std::string>{"legacy"});
    EXPECT_TRUE(gateway.validated.empty());
}

TEST(DiagnosticsPaths, ValidateDriverConfigs_RequireValidation_ChecksEarlierDomainFirst) {
    // The unchecked driver is the *second* domain: the sweep validates the
    // first one and only then refuses to continue.
    ScriptedGateway gateway;
    gateway.unsupported = {"legacy"};
    const domain::RuntimeConfig config{
        .domains =
            {
                make_domain("a.example.com", "fake", {make_subdomain("@", "{}")}),
                make_domain("b.example.com", "legacy", {make_subdomain("@", "{}")}),
            },
    };
    coro::Loop loop;

    const auto outcome = coro::run(
        loop, app::validate_driver_configs(gateway, config, app::ConfigValidationMode::REQUIRE_VALIDATION, 5s));

    EXPECT_EQ(outcome.rejection,
              "Driver 'legacy' does not provide yaddnsc_driver_validate; its configuration was not checked");
    EXPECT_EQ(gateway.validated, (ValidatedCalls{{"fake", "{}"}}));
}
