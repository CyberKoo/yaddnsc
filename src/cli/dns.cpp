//
// Created by Kotarou on 2026/6/30.
//

#include "dns.h"

#include <cstdlib>
#include <print>
#include <memory>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "fmt.hpp"
#include "uri.h"
#include "config/config.h"
#include "dns/dispatcher.h"
#include "dns/factory.h"
#include "record_kind.h"

namespace Cli {
    // ── Option storage (owned by the callback lambda via shared_ptr) ──────

    namespace {
        struct DnsOpts {
            std::string config_path = "config.json";
            std::string dns_host;
            std::string dns_type = "A";
        };
    }

        void register_dns_subcommand(CLI::App &app, int &exit_code) {
            auto opts = std::make_shared<DnsOpts>();

            auto *dns = app.add_subcommand("dns", "DNS lookup and diagnostics");
            dns->require_subcommand(1);
            dns->add_option("-c,--config", opts->config_path, "Config file path")
                ->default_str("config.json")->check(CLI::ExistingFile);

            auto *resolve = dns->add_subcommand("resolve", "Resolve a hostname");
            resolve->alias("r");
            resolve->add_option("hostname", opts->dns_host, "Hostname to resolve (e.g. example.com)")->required();
            resolve->add_option("--type", opts->dns_type, "Record type (A, AAAA, TXT)")
                    ->default_str("A")
                    ->check(CLI::IsMember(std::vector<std::string>{"A", "AAAA", "TXT"}));
            resolve->callback([&exit_code, opts] {
                exit_code = execute_dns_resolve(opts->config_path, opts->dns_host, opts->dns_type);
            });

            auto *resolver = dns->add_subcommand("resolver", "Show configured resolver details");
            resolver->callback([&exit_code, opts] {
                exit_code = execute_dns_resolver(opts->config_path);
            });
        }

    // ── Executors ─────────────────────────────────────────────────────────

    int execute_dns_resolve(const std::string &config_path, const std::string &host, const std::string &type_str) {
        auto type = magic_enum::enum_cast<RecordKind>(type_str, magic_enum::case_insensitive);
        if (!type.has_value()) {
            std::print(std::cerr, "Error: unknown record type '{}'.\nValid types: ", type_str);
            const auto names = magic_enum::enum_names<RecordKind>();
            for (auto it = names.begin(); it != names.end(); ++it) {
                if (it != names.begin()) { std::print(std::cerr, ", "); }
                std::print(std::cerr, "{}", *it);
            }
            std::println(std::cerr, "");
            return EXIT_FAILURE;
        }

        auto config = Config::load_config(config_path);
        auto resolver = DnsResolverFactory::create(config);
        auto dns_result = resolver.resolve(host, *type);

        if (dns_result.empty()) {
            std::println("DNS lookup for {} ({}) failed: no records found", host, type_str);
            return EXIT_SUCCESS;
        }

        std::println("DNS lookup result:\n"
                     "  Host:  {}\n"
                     "  Type:  {}\n"
                     "  Value: {}", host, type_str, fmt::format("{}", fmt::join(dns_result, ", "))
        );
        return EXIT_SUCCESS;
    }

    int execute_dns_resolver(const std::string &config_path) {
        auto config = Config::load_config(config_path);
        const auto &resolver = config.resolver;

        std::println("DNS resolver configuration:\n"
                     "  Custom server: {}\n"
                     "  Strategy:      {}",
                     resolver.use_custom_server ? "yes" : "no",
                     magic_enum::enum_name(resolver.strategy)
        );

        if (!resolver.servers.empty()) {
            std::println("  Servers ({}):", resolver.servers.size());
            for (const auto &srv: resolver.servers) {
                const auto uri = Uri::parse(srv.address);
                if (!uri.get_schema().empty()) {
                    std::string display = uri.get_origin();
                    auto path = uri.get_path();
                    if (!path.empty() && path != "/") {
                        display += path;
                    }
                    std::println("    - {}", display);
                } else {
                    std::println("    - {}:{}", uri.get_host_literal(), srv.port);
                }
            }
        } else if (resolver.use_custom_server && !resolver.address.empty()) {
            std::println("  Server: {}:{}", resolver.address, resolver.port);
        }

        return EXIT_SUCCESS;
    }
} // namespace Cli
