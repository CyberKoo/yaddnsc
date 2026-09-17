//
// Created by Kotarou on 2026/9/17.
//

#include "update_workflow.h"

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "domain/address_policy.h"
#include "domain/update/update_task.h"

#include <magic_enum/magic_enum.hpp>

UpdateWorkflow::UpdateWorkflow(const DnsResolverPort &dns_resolver, const IpSourcePort &ip_source,
                               const DriverGateway &driver_gateway, const Logger &logger)
    : dns_resolver_(dns_resolver), ip_source_(ip_source), driver_gateway_(driver_gateway), logger_(logger) {
}

UpdateOutcome UpdateWorkflow::run(const domain::UpdateTask &task) const {
    try {
        const auto &subdomain = task.subdomain_config();
        const auto rd_type_name = magic_enum::enum_name(subdomain.type);
        const auto rd_type = rd_type_name.empty() ? "UNKNOWN" : rd_type_name;

        // --- Step 1: local IP -------------------------------------------------

        const auto candidates = ip_source_.resolve(subdomain);
        if (!candidates) {
            YLOG_ERROR(logger_, "Failed to resolve local IP address for {}, skipping the update: {}", task.fqdn,
                       candidates.error().message);
            return std::unexpected(
                domain::UpdateError{domain::UpdateError::Code::SKIPPED_NO_ADDRESS, candidates.error().message});
        }

        const auto local_ip =
            domain::select_address(*candidates, subdomain.type, {subdomain.allow_ula, subdomain.allow_local_link});
        if (!local_ip) {
            YLOG_WARN(logger_, "No valid IP address found for {}, skipping the update", task.fqdn);
            return std::unexpected(
                domain::UpdateError{domain::UpdateError::Code::SKIPPED_NO_ADDRESS, "No valid IP address found"});
        }

        // --- Step 2: current DNS records (unless force_update) ----------------
        // A failed lookup never blocks the update: it is mapped to an empty
        // record list, which decides the same as a successful empty answer.

        std::vector<std::string> records;
        if (!task.force_update) {
            const auto resolved = dns_resolver_.resolve(task.fqdn, subdomain.type);
            if (!resolved) {
                // Cannot verify the current record (transient failure, NXDOMAIN,
                // NODATA, ...) — proceed with the update anyway: pushing an
                // unchanged record is harmless, while skipping a changed one is not.
                YLOG_DEBUG(logger_, R"(DNS lookup for "{}" failed: {} ({}), proceeding with update)", task.fqdn,
                           resolved.error().message, error_to_str(resolved.error().code));
            } else {
                records = *resolved;
            }
        }

        // --- Step 3: decision (only the FIRST record is compared) -------------

        const auto local_addr = local_ip->to_string();
        const auto decision = domain::decide_update(records, local_addr, task.force_update);
        switch (decision) {
            case domain::UpdateDecision::SkipNoAddress:
                // Unreachable: step 1 already returned. Kept for exhaustive
                // switching over the decision contract.
                return std::unexpected(
                    domain::UpdateError{domain::UpdateError::Code::SKIPPED_NO_ADDRESS, "No valid IP address found"});
            case domain::UpdateDecision::SkipUnchanged:
                YLOG_DEBUG(logger_, "Domain {} ({}) unchanged ({}), skipping update", task.fqdn, rd_type,
                           records.front());
                return UpdateResult{decision};
            case domain::UpdateDecision::UpdateChanged:
                if (!records.empty()) {
                    YLOG_DEBUG(logger_, "Domain {} ({}) will be updated to {} (was {})", task.fqdn, rd_type,
                               local_addr, records.front());
                }
                break;
            case domain::UpdateDecision::UpdateForced:
                YLOG_INFO(logger_, "Force update triggered for {}", task.fqdn);
                break;
        }

        // --- Step 4: delegate to the driver gateway ---------------------------

        const DriverUpdateCommand command{
            .driver_param = subdomain.driver_param,
            .ip_addr = local_addr,
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
            return std::unexpected(domain::UpdateError{domain::UpdateError::Code::DRIVER_FAILED,
                                                       result.error().message, result.error().retry_after_seconds});
        }

        YLOG_INFO(logger_, "Domain {} ({}) updated to {}", task.fqdn, rd_type, local_addr);
        return UpdateResult{decision};
    } catch (const std::exception &e) {
        // Defence against unexpected exceptions only — expected failures are
        // error values handled above.
        YLOG_ERROR(logger_, "Unhandled exception during update of {}. {}", task.fqdn, e.what());
        return std::unexpected(domain::UpdateError{domain::UpdateError::Code::UNKNOWN, e.what()});
    } catch (...) {
        YLOG_ERROR(logger_, "Unknown non-standard exception during update for {}", task.fqdn);
        return std::unexpected(
            domain::UpdateError{domain::UpdateError::Code::UNKNOWN, "Unknown non-standard exception"});
    }
}
