//
// Created by Kotarou on 2026/9/17.
//

#include "presenter.h"

#include <cstdlib>
#include <iostream>
#include <print>
#include <string_view>

#include <magic_enum/magic_enum.hpp>

#include "domain/network/address_family.h"
#include "support/fmt.hpp"
#include "domain/dns/record_kind.h"

#include "build_id.hpp"
#include "min_update_interval.h"
#include "resolver_config.h"
#include "version.h"


int Cli::present_driver_list(const std::vector<Diagnostics::DriverListItem> &items) {
    if (items.empty()) {
        std::println("No drivers loaded.");
        return EXIT_SUCCESS;
    }

    std::println("Loaded drivers ({}):", items.size());
    for (const auto &item: items) {
        if (item.detail.has_value()) {
            const auto &detail = *item.detail;
            std::println("  {} — {} (v{}, by {})", detail.name, detail.description, detail.version, detail.author);
        } else {
            std::println("  {} — (failed to query details: {})", item.name, item.error);
        }
    }
    return EXIT_SUCCESS;
}

int Cli::present_driver_info(const DriverDescription &detail) {
    std::println(
        "Name:        {}\n"
        "Description: {}\n"
        "Author:      {}\n"
        "Version:     {}",
        detail.name, detail.description, detail.author, detail.version);
    return EXIT_SUCCESS;
}

int Cli::present_interface_list(const std::vector<Diagnostics::InterfaceListItem> &items) {
    if (items.empty()) {
        std::println("No network interfaces found.");
        return EXIT_SUCCESS;
    }

    std::println("Network interfaces ({}):", items.size());
    for (const auto &item: items) {
        std::print("  {}", item.name);
        if (!item.addresses.empty()) {
            std::print(" (");
            for (size_t i = 0; i < item.addresses.size(); ++i) {
                if (i > 0) {
                    std::print(", ");
                }
                std::print("{}", item.addresses[i].to_string());
            }
            std::print(")");
        }
        std::println("");
    }
    return EXIT_SUCCESS;
}

int Cli::present_interface_ip(const std::string &name, const std::vector<InetAddress> &addresses) {
    std::println("Interface: {}", name);
    for (const auto &addr: addresses) {
        std::println("  {} ({})", addr.to_string(), addr.get_family() == AddressFamily::IPV4 ? "IPv4" : "IPv6");
    }
    return EXIT_SUCCESS;
}

int Cli::present_dns_resolve(const Diagnostics::DnsResolveOutcome &outcome) {
    if (!outcome.lookup.has_value()) {
        std::print(std::cerr, "Error: unknown record type '{}'.\nValid types: ", outcome.type_text);
        const auto names = magic_enum::enum_names<RecordKind>();
        for (auto it = names.begin(); it != names.end(); ++it) {
            if (it != names.begin()) {
                std::print(std::cerr, ", ");
            }
            std::print(std::cerr, "{}", *it);
        }
        std::println(std::cerr, "");
        return EXIT_FAILURE;
    }

    const auto &lookup = *outcome.lookup;
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

int Cli::present_dns_resolver(const bool use_custom_server, const std::string_view strategy,
                              const std::vector<std::string> &servers, const std::string_view legacy_address,
                              const unsigned short legacy_port) {
    std::println(
        "DNS resolver configuration:\n"
        "  Custom server: {}\n"
        "  Strategy:      {}",
        use_custom_server ? "yes" : "no", strategy);

    if (!servers.empty()) {
        std::println("  Servers ({}):", servers.size());
        for (const auto &server: servers) {
            std::println("    - {}", server);
        }
    } else if (use_custom_server && !legacy_address.empty()) {
        std::println("  Server: {}:{}", legacy_address, legacy_port);
    }

    return EXIT_SUCCESS;
}

int Cli::present_config_show(const std::string_view json) {
    std::println("{}", json);
    return EXIT_SUCCESS;
}

int Cli::present_config_test(const Diagnostics::ConfigTestOutcome &outcome) {
    if (!outcome.error.has_value()) {
        if (!outcome.quiet) {
            std::println("Configuration file test passed");
        }
        return EXIT_SUCCESS;
    }

    const auto &error = *outcome.error;
    switch (error.kind) {
        case Diagnostics::ConfigTestError::Kind::VERIFICATION:
            std::println(std::cerr, "Configuration verification failed: {}", error.message);
            break;
        case Diagnostics::ConfigTestError::Kind::FATAL:
            std::println(std::cerr, "Fatal error: unrecoverable exception: {}", error.message);
            break;
        case Diagnostics::ConfigTestError::Kind::GENERIC:
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

int Cli::present_error(const std::exception &e) {
    std::println(std::cerr, "Error: {}", e.what());
    return EXIT_FAILURE;
}
