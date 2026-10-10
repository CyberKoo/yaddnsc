#ifndef YADDNSC_DOMAIN_UPDATE_UPDATE_TASK_H
#define YADDNSC_DOMAIN_UPDATE_UPDATE_TASK_H

#include <string>

#include "domain/config/runtime_config.h"

namespace domain {

/// UpdateTask — a self-contained value type describing one DNS record update
///              that the update workflow should carry out.
///
/// Borrows the immutable configuration for one record update. A subdomain loop
/// owns one task and reuses its FQDN across cycles; only force_update changes,
/// after the previous update has completed.
struct UpdateTask {
    const DomainConfig& domain;
    const SubdomainConfig& subdomain;
    std::string fqdn;                             ///< Fully qualified domain name
    bool force_update{false};                     ///< Skip IP-change check; always send update
};

}  // namespace domain

#endif  // YADDNSC_DOMAIN_UPDATE_UPDATE_TASK_H
