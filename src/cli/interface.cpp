//
// Created by Kotarou on 2026/6/30.
//

#include "interface.h"

#include <cstdlib>
#include <memory>

#include "ip_source/iface_util.h"
#include "network/inet_address.h"

#include <print>

namespace Cli {
    // ── Option storage (owned by the callback lambda via shared_ptr) ──────

    namespace {
        struct InterfaceOpts {
            std::string interface_name;
        };
    } // namespace

    void register_interface_subcommand(CLI::App &app, int &exit_code) {
        auto opts = std::make_shared<InterfaceOpts>();

        auto *iface = app.add_subcommand("interface", "Query network interfaces");
        iface->alias("if");
        iface->alias("net");
        iface->require_subcommand(1);

        auto *list = iface->add_subcommand("list", "List all network interfaces");
        list->callback([&exit_code] { exit_code = execute_interface_list(); });

        auto *ip = iface->add_subcommand("ip", "Show IP addresses of a network interface");
        ip->add_option("name", opts->interface_name, "Interface name (e.g. eth0, en0)")->required();
        ip->callback([&exit_code, opts] { exit_code = execute_interface_ip(opts->interface_name); });
    }

    // ── Executors ─────────────────────────────────────────────────────────

    int execute_interface_list() {
        const auto interfaces = InterfaceUtil::get_interfaces();
        if (interfaces.empty()) {
            std::println("No network interfaces found.");
            return EXIT_SUCCESS;
        }

        std::println("Network interfaces ({}):", interfaces.size());
        for (const auto &name: interfaces) {
            const auto addrs = InterfaceUtil::get_addresses(name);
            std::print("  {}", name);
            if (!addrs.empty()) {
                std::print(" (");
                for (size_t i = 0; i < addrs.size(); ++i) {
                    if (i > 0) {
                        std::print(", ");
                    }
                    std::print("{}", addrs[i].to_string());
                }
                std::print(")");
            }
            std::println("");
        }
        return EXIT_SUCCESS;
    }

    int execute_interface_ip(const std::string &interface_name) {
        try {
            const auto addrs = InterfaceUtil::get_addresses(interface_name);
            std::println("Interface: {}", interface_name);
            for (const auto &addr: addrs) {
                std::println("  {} ({})", addr.to_string(), addr.get_family() == AddressFamily::IPV4 ? "IPv4" : "IPv6");
            }
        } catch (const std::exception &e) {
            std::println(std::cerr, "Error: {}", e.what());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
} // namespace Cli
