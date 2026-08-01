//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_CORE_UPDATE_TASK_HPP
#define YADDNSC_CORE_UPDATE_TASK_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "config/config.h"

/// UpdateTask — a self-contained value type describing one DNS record update
///              that the Updater should carry out.
///
/// The task shares the application configuration via `shared_ptr` and
/// references the target domain/subdomain by index, so copying a task (which
/// happens on every scheduler pop) is cheap: no per-task copy of the config,
/// including its JSON driver parameters.
struct UpdateTask {
    std::shared_ptr<const Config::AppConfig> config;  ///< Shared application configuration
    std::size_t domain_index{};                        ///< Index into config->domains
    std::size_t subdomain_index{};                     ///< Index into domains[domain_index].subdomains
    std::string fqdn;                                  ///< Fully qualified domain name
    bool force_update{false};                          ///< Skip IP-change check; always send update

    /// The subdomain configuration this task updates.
    [[nodiscard]] const Config::SubdomainConfig &subdomain_config() const {
        return config->domains[domain_index].subdomains[subdomain_index];
    }

    /// Name of the parent domain.
    [[nodiscard]] std::string_view domain_name() const {
        return config->domains[domain_index].name;
    }

    /// Name of the driver plugin to use.
    [[nodiscard]] std::string_view driver_name() const {
        return config->domains[domain_index].driver;
    }
};

#endif //YADDNSC_CORE_UPDATE_TASK_HPP
