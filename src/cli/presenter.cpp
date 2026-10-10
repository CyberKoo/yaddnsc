#include "presenter.h"

#include <magic_enum/magic_enum.hpp>
#include <yaddnsc/util/format.hpp>
#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <ranges>
#include <string_view>
#include <expected>
#include <print>

#include "application/diagnostics.h"
#include "domain/config/dns_config.h"
#include "domain/dns/record_kind.h"  // IWYU pragma: keep — magic_enum::enum_names needs the definition
#include "domain/error/dns_error_info.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/uri/uri.h"
#include "support/fmt.hpp"
#include "build_id.hpp"
#include "min_update_interval.h"
#include "resolver_config.h"
#include "version.h"
#include "application/ports/driver_catalog.h"
#include "domain/error/error.h"

namespace {
std::string driver_description_error(std::string_view name, const domain::DriverError& error) {
    if (error.code == domain::DriverError::Code::NOT_FOUND) {
        return fmt::format("Driver '{}' is not loaded", name);
    }
    return error.message;
}
}  // namespace

int Cli::present_driver_list(const std::vector<app::DriverListItem>& items) {
    if (items.empty()) {
        std::println("No drivers loaded.");
        return EXIT_SUCCESS;
    }

    std::println("Loaded drivers ({}):", items.size());
    for (const auto& item : items) {
        if (item.detail.has_value()) {
            const auto& detail = *item.detail;
            std::println("  {} — {} (v{}, by {})", detail.name, detail.description, detail.version, detail.author);
        } else {
            std::println("  {} — (failed to query details: {})", item.name,
                         driver_description_error(item.name, item.detail.error()));
        }
    }
    return EXIT_SUCCESS;
}

int Cli::present_driver_info(std::string_view name,
                             const std::expected<app::DriverDescription, domain::DriverError>& result) {
    if (!result) {
        std::println(std::cerr, "Error: {}", driver_description_error(name, result.error()));
        return EXIT_FAILURE;
    }
    const auto& detail = *result;
    std::println(
        "Name:        {}\n"
        "Description: {}\n"
        "Author:      {}\n"
        "Version:     {}",
        detail.name, detail.description, detail.author, detail.version);
    return EXIT_SUCCESS;
}

int Cli::present_interface_list(const std::vector<app::InterfaceListItem>& items) {
    if (items.empty()) {
        std::println("No network interfaces found.");
        return EXIT_SUCCESS;
    }

    std::println("Network interfaces ({}):", items.size());
    for (const auto& item : items) {
        std::print("  {}", item.name);
        if (!item.addresses.empty()) {
            const auto address_strings = item.addresses | std::views::transform([](const domain::InetAddress& addr) {
                                             return addr.to_string();
                                         });
            std::print(" ({})", fmt::format("{}", fmt::join(address_strings, ", ")));
        }
        std::println("");
    }
    return EXIT_SUCCESS;
}

int Cli::present_interface_ip(const std::string& name,
                              const std::optional<std::vector<domain::InetAddress>>& addresses) {
    if (!addresses.has_value()) {
        std::println(std::cerr, "Error: Interface {} not found", name);
        return EXIT_FAILURE;
    }
    std::println("Interface: {}", name);
    for (const auto& addr : *addresses) {
        std::println("  {} ({})", addr.to_string(), addr.get_family() == domain::AddressFamily::IPV4 ? "IPv4" : "IPv6");
    }
    return EXIT_SUCCESS;
}

int Cli::present_dns_resolve(const app::DnsResolveOutcome& outcome) {
    if (!outcome.lookup.has_value()) {
        std::print(std::cerr, "Error: unknown record type '{}'.\nValid types: ", outcome.type_text);
        const auto names = magic_enum::enum_names<domain::RecordKind>();
        for (auto it = names.begin(); it != names.end(); ++it) {
            if (it != names.begin()) {
                std::print(std::cerr, ", ");
            }
            std::print(std::cerr, "{}", *it);
        }
        std::println(std::cerr, "");
        return EXIT_FAILURE;
    }

    const auto& lookup = *outcome.lookup;
    if (!lookup.has_value()) {
        std::println("DNS lookup for {} ({}) failed: {}", outcome.host, outcome.type_text, lookup.error().message);
        return EXIT_SUCCESS;
    }

    if (lookup->empty()) {
        std::println("DNS lookup for {} ({}) returned no records", outcome.host, outcome.type_text);
        return EXIT_SUCCESS;
    }

    std::println(
        "DNS lookup result:\n"
        "  Host:  {}\n"
        "  Type:  {}\n"
        "  Value: {}",
        outcome.host, outcome.type_text, fmt::format("{}", fmt::join(*lookup, ", ")));
    return EXIT_SUCCESS;
}

namespace {
[[nodiscard]] std::string format_resolver_server(const domain::DnsServer& server) {
    const auto uri = Uri::parse(server.address);
    if (!uri.has_value()) {
        // Display helper must never fail: show the raw address as-is.
        return server.address;
    }
    if (!uri->get_schema().empty()) {
        std::string display = uri->get_origin();
        const auto path = uri->get_path();
        if (!path.empty() && path != "/") {
            display += path;
        }
        return display;
    }
    return fmt::format("{}:{}", uri->get_host_literal(), server.port);
}

}  // namespace

int Cli::present_dns_resolver(const bool use_custom_servers, const std::string_view strategy,
                              const std::vector<domain::DnsServer>& servers) {
    std::println(
        "DNS resolver configuration:\n"
        "  Custom server: {}\n"
        "  Strategy:      {}",
        use_custom_servers ? "yes" : "no", strategy);

    if (!servers.empty()) {
        std::println("  Servers ({}):", servers.size());
        for (const auto& server : servers) {
            std::println("    - {}", format_resolver_server(server));
        }
    }

    return EXIT_SUCCESS;
}

int Cli::present_config_show(const std::string_view json) {
    std::println("{}", json);
    return EXIT_SUCCESS;
}

int Cli::present_config_test(const app::ConfigTestOutcome& outcome) {
    if (!outcome.error.has_value()) {
        if (!outcome.quiet) {
            std::println("Configuration file test passed");
        }
        return EXIT_SUCCESS;
    }

    const auto& error = *outcome.error;
    switch (error.kind) {
        case app::ConfigTestError::Kind::VERIFICATION:
            // The message may carry several collected errors, one per line;
            // repeat the prefix so every line reads as a complete statement.
            for (std::string_view rest = error.message; !rest.empty();) {
                const auto newline = rest.find('\n');
                const auto line = rest.substr(0, newline);
                std::println(std::cerr, "Configuration verification failed: {}", line);
                rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
            }
            break;
        case app::ConfigTestError::Kind::FATAL:
            std::println(std::cerr, "Fatal error: unrecoverable exception: {}", error.message);
            break;
        case app::ConfigTestError::Kind::GENERIC:
            std::println(std::cerr, "Failed to validate configuration: {}", error.message);
            break;
    }
    return EXIT_FAILURE;
}

int Cli::present_info() {
    std::println("Build configuration:");
    std::println("  {:<20} {}", "Version:", YADDNSC::get_full_version());
    std::println("  {:<20} {}", "Build ID:", BuildId::full_id());
    std::println("  {:<20} {}", "C library:", BuildId::LIBC_TYPE);
    if (BuildId::GLIBCXX_CXX11_ABI) {
        std::println("  {:<20} {} (_GLIBCXX_USE_CXX11_ABI=1)", "Compiler ABI:", BuildId::COMPILER_ABI);
    } else {
        std::println("  {:<20} {}", "Compiler ABI:", BuildId::COMPILER_ABI);
    }
    std::println("  {:<20} 0x{:016X}", "Compiler ID hash:", BuildId::COMPILER_ID_HASH);

    std::println("  {:<20} C++{}", "C++ standard:", __cplusplus / 100 % 100);

    std::println("  {:<20} {}", "DNS resolver:", "Native (built-in)");

    std::println("  {:<20} {}:{}", "Default DNS:", YADDNSC_DEFAULT_DNS_SERVER, YADDNSC_DEFAULT_DNS_PORT);

    std::println("  {:<20} {}s", "Min update interval:", YADDNSC_MIN_UPDATE_INTERVAL);

#ifdef YADDNSC_USE_STD_FORMAT
    std::println("  {:<20} {}", "Format library:", "std::format");
#else
    std::println("  {:<20} {}", "Format library:", "fmt");
#endif

#ifdef YADDNSC_USE_SYSTEM_SPDLOG
    std::println("  {:<20} {}", "spdlog:", "system package");
#else
    std::println("  {:<20} {}", "spdlog:", "bundled");
#endif

    return EXIT_SUCCESS;
}

int Cli::present_error(const std::exception& e) {
    std::println(std::cerr, "Error: {}", e.what());
    return EXIT_FAILURE;
}
