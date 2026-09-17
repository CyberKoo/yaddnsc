//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_DOMAIN_UPDATE_UPDATE_TASK_H
#define YADDNSC_DOMAIN_UPDATE_UPDATE_TASK_H

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "domain/config/runtime_config.h"

namespace domain {

/// UpdateTask — a self-contained value type describing one DNS record update
///              that the update workflow should carry out.
///
/// The task shares the runtime configuration via `shared_ptr` and
/// references the target domain/subdomain by index, so copying a task (which
/// happens on every schedule pop) is cheap: no per-task copy of the config,
/// including its JSON driver parameters.
struct UpdateTask {
    std::shared_ptr<const RuntimeConfig> config;  ///< Shared runtime configuration
    std::size_t domain_index{};                   ///< Index into config->domains
    std::size_t subdomain_index{};                ///< Index into domains[domain_index].subdomains
    std::string fqdn;                             ///< Fully qualified domain name
    bool force_update{false};                     ///< Skip IP-change check; always send update

    /// The subdomain configuration this task updates.
    [[nodiscard]] const SubdomainConfig &subdomain_config() const {
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

} // namespace domain

#endif // YADDNSC_DOMAIN_UPDATE_UPDATE_TASK_H
