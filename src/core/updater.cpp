//
// Created by Kotarou on 2026/6/18.
//

#include "updater.h"

#include <exception>
#include <utility>

#include "domain/address_policy.h"
#include "update_task.hpp"

#include <magic_enum/magic_enum.hpp>

Updater::Updater(const DnsResolverPort &dns_resolver, const IpSourcePort &ip_source,
                 const DriverGateway &driver_gateway, const Logger &logger)
    : dns_resolver_(dns_resolver), ip_source_(ip_source), driver_gateway_(driver_gateway), logger_(logger) {
}

void Updater::process(const UpdateTask &task) const noexcept {
    try {
        const auto &subdomain = task.subdomain_config();
        const auto rd_type_name = magic_enum::enum_name(subdomain.type);
        const auto rd_type = rd_type_name.empty() ? "UNKNOWN" : rd_type_name;

        // --- Step 1: local IP -------------------------------------------------

        const auto candidates = ip_source_.resolve(subdomain);
        if (!candidates) {
            YLOG_ERROR(logger_, "Failed to resolve local IP address for {}, skipping the update: {}", task.fqdn,
                       candidates.error().message);
            return;
        }

        const auto local_ip =
            domain::select_address(*candidates, subdomain.type, {subdomain.allow_ula, subdomain.allow_local_link});
        if (!local_ip) {
            YLOG_WARN(logger_, "No valid IP address found for {}, skipping the update", task.fqdn);
            return;
        }

        // --- Step 2: skip if unchanged (unless force_update) ------------------

        if (!task.force_update) {
            const auto records = dns_resolver_.resolve(task.fqdn, subdomain.type);

            if (!records) {
                // Cannot verify the current record (transient failure, NXDOMAIN,
                // NODATA, ...) — proceed with the update anyway: pushing an
                // unchanged record is harmless, while skipping a changed one is not.
                YLOG_DEBUG(logger_, R"(DNS lookup for "{}" failed: {} ({}), proceeding with update)", task.fqdn,
                           records.error().message, error_to_str(records.error().code));
            } else if (!records->empty()) {
                const auto &first = records->front();
                if (first == local_ip->to_string()) {
                    YLOG_DEBUG(logger_, "Domain {} ({}) unchanged ({}), skipping update", task.fqdn, rd_type, first);
                    return;
                }

                YLOG_DEBUG(logger_, "Domain {} ({}) will be updated to {} (was {})", task.fqdn, rd_type,
                           local_ip->to_string(), first);
            }
        } else {
            YLOG_INFO(logger_, "Force update triggered for {}", task.fqdn);
        }

        // --- Step 3: delegate to the driver gateway ----------------------------

        const DriverUpdateCommand command{
            .driver_param = subdomain.driver_param,
            .ip_addr = local_ip->to_string(),
            .rd_type = std::string(rd_type),
            .domain = std::string(task.domain_name()),
            .subdomain = subdomain.name,
            .fqdn = task.fqdn,
        };

        const auto result = driver_gateway_.update(task.driver_name(), command);
        if (!result) {
            switch (result.error().code) {
                case domain::DriverError::Code::NOT_FOUND:
                    YLOG_ERROR(logger_, "Driver '{}' not found for task '{}', skipping: {}", task.driver_name(),
                               task.fqdn, result.error().message);
                    break;
                case domain::DriverError::Code::UNKNOWN:
                    // Exception escaped the driver (e.g. parameter parse failure) —
                    // same wording the legacy catch-all produced.
                    YLOG_ERROR(logger_, "Unhandled exception during update of {}. {}", task.fqdn,
                               result.error().message);
                    break;
                default:
                    // The driver already logged the upstream/HTTP failure itself.
                    YLOG_DEBUG(logger_, "{}", result.error().message);
                    break;
            }
            return;
        }

        YLOG_INFO(logger_, "Domain {} ({}) updated to {}", task.fqdn, rd_type, local_ip->to_string());
    } catch (const std::exception &e) {
        // Defence against unexpected exceptions only — expected failures are
        // error values handled above.
        YLOG_ERROR(logger_, "Unhandled exception during update of {}. {}", task.fqdn, e.what());
    } catch (...) {
        YLOG_ERROR(logger_, "Unknown non-standard exception during update for {}", task.fqdn);
    }
}
