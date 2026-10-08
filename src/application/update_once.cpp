//
// app — one coroutine update cycle (implementation).
//

#include "update_once.h"

#include <chrono>
#include <cstddef>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "application/ports/log.h"
#include "domain/address_policy.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/inet_address.h"
#include "infrastructure/coro/scope.hpp"
#include "support/fmt.hpp"

namespace app {

coro::Task<UpdateOnceOutcome> update_once(const domain::UpdateTask& task, const Services& services) {
    const auto& subdomain = task.subdomain_config();
    const auto rd_type_name = magic_enum::enum_name(subdomain.type);
    const std::string_view rd_type = rd_type_name.empty() ? std::string_view{"UNKNOWN"} : rd_type_name;

    try {
        // --- Step 1: local IP -------------------------------------------------

        const auto candidates = co_await services.ip_source.resolve(subdomain);
        if (!candidates) {
            if (candidates.error().code == domain::IpSourceError::Code::CANCELLED) {
                co_return std::unexpected(
                    domain::UpdateError{domain::UpdateError::Code::CANCELLED, candidates.error().message});
            }
            YLOG_ERROR(services.logger, "Failed to resolve local IP address for {}, skipping the update: {}", task.fqdn,
                       candidates.error().message);
            co_return std::unexpected(
                domain::UpdateError{domain::UpdateError::Code::SKIPPED_NO_ADDRESS, candidates.error().message});
        }

        const auto local_ip =
            domain::select_address(*candidates, subdomain.type, {subdomain.allow_ula, subdomain.allow_local_link});
        if (!local_ip) {
            YLOG_WARN(services.logger, "No valid IP address found for {}, skipping the update", task.fqdn);
            co_return std::unexpected(
                domain::UpdateError{domain::UpdateError::Code::SKIPPED_NO_ADDRESS, "No valid IP address found"});
        }

        // --- Step 2: current DNS records (unless force_update) ----------------
        // A failed lookup never blocks the update: it is mapped to an empty
        // record list, which decides the same as a successful empty answer.

        std::vector<std::string> records;
        if (!task.force_update) {
            // The read gets its own budget so a resolver that never answers
            // surfaces in seconds instead of holding the whole cycle to
            // UPDATE_BUDGET. Timeout is a scope property composed here, not an
            // I/O parameter — the new exchange layer has none.
            const auto read = co_await coro::with_timeout(
                DNS_READ_BUDGET,
                [&services, &task, &subdomain](coro::CancelScope&)
                    -> coro::Task<std::expected<std::vector<std::string>, DnsErrorInfo>> {
                    co_return co_await services.resolver.resolve(task.fqdn, subdomain.type);
                });

            if (read.timed_out) {
                // Our own budget fired (timed_out implies self-cancellation, and
                // it is checked before `cancelled` because the timer also marks
                // the scope cancelled): a transient read failure, not an abort —
                // proceed with the update on an empty record list.
                YLOG_DEBUG(services.logger, R"(DNS lookup for "{}" failed: timed out after {}s, proceeding with update)",
                           task.fqdn, std::chrono::duration_cast<std::chrono::seconds>(DNS_READ_BUDGET).count());
            } else if (read.cancelled) {
                // The enclosing scope fired (shutdown/abort): propagate, publish
                // nothing.
                co_return std::unexpected(
                    domain::UpdateError{domain::UpdateError::Code::CANCELLED, "DNS lookup cancelled"});
            } else if (read.has_value()) {
                const auto& resolved = *read;
                if (!resolved) {
                    if (resolved.error().code == DnsError::CANCELLED) {
                        co_return std::unexpected(
                            domain::UpdateError{domain::UpdateError::Code::CANCELLED, resolved.error().message});
                    }
                    // Cannot verify the current record (transient failure, NXDOMAIN,
                    // NODATA, ...) — proceed with the update anyway: pushing an
                    // unchanged record is harmless, while skipping a changed one is not.
                    YLOG_DEBUG(services.logger, R"(DNS lookup for "{}" failed: {} ({}), proceeding with update)",
                               task.fqdn, resolved.error().message, error_to_str(resolved.error().code));
                } else {
                    records = *resolved;
                }
            }
        }

        // --- Step 3: decision (only the FIRST record is compared) -------------

        const auto local_addr = local_ip->to_string();
        const auto decision = domain::decide_update(records, local_addr, task.force_update);
        switch (decision) {
            case domain::UpdateDecision::SKIP_NO_ADDRESS:
                // Unreachable: step 1 already returned. Kept for exhaustive
                // switching over the decision contract.
                co_return std::unexpected(
                    domain::UpdateError{domain::UpdateError::Code::SKIPPED_NO_ADDRESS, "No valid IP address found"});
            case domain::UpdateDecision::SKIP_UNCHANGED:
                YLOG_DEBUG(services.logger, "Domain {} ({}) unchanged ({}), skipping update", task.fqdn, rd_type,
                           records.front());
                co_return UpdateCycleResult{decision};
            case domain::UpdateDecision::UPDATE_CHANGED:
                if (!records.empty()) {
                    YLOG_DEBUG(services.logger, "Domain {} ({}) will be updated to {} (was {})", task.fqdn, rd_type,
                               local_addr, records.front());
                }
                break;
            case domain::UpdateDecision::UPDATE_FORCED:
                YLOG_INFO(services.logger, "Force update triggered for {}", task.fqdn);
                break;
        }

        // --- Step 4: delegate to the driver gateway ---------------------------

        const domain::DriverUpdateCommand command{
            .driver_params = subdomain.driver_params,
            .ip_addr = local_addr,
            .rd_type = std::string(rd_type),
            .domain = std::string(task.domain_name()),
            .subdomain = subdomain.name,
            .fqdn = task.fqdn,
        };

        const auto result = co_await services.gateway.update(std::string(task.driver_name()), command);
        if (!result) {
            switch (result.error().code) {
                case domain::DriverError::Code::CANCELLED:
                    co_return std::unexpected(
                        domain::UpdateError{domain::UpdateError::Code::CANCELLED, result.error().message});
                case domain::DriverError::Code::NOT_FOUND:
                    YLOG_ERROR(services.logger, "Driver '{}' not found for task '{}', skipping: {}",
                               task.driver_name(), task.fqdn, result.error().message);
                    break;
                case domain::DriverError::Code::UNKNOWN:
                    // Exception escaped the driver (e.g. parameter parse failure) —
                    // same wording the legacy catch-all produced.
                    YLOG_ERROR(services.logger, "Unhandled exception during update of {}. {}", task.fqdn,
                               result.error().message);
                    break;
                default:
                    // The driver already logged the upstream/HTTP failure itself.
                    YLOG_DEBUG(services.logger, "{}", result.error().message);
                    break;
            }
            co_return std::unexpected(domain::UpdateError{domain::UpdateError::Code::DRIVER_FAILED,
                                                          result.error().message,
                                                          result.error().retry_after_seconds});
        }

        YLOG_INFO(services.logger, "Domain {} ({}) updated to {}", task.fqdn, rd_type, local_addr);
        co_return UpdateCycleResult{decision};
    } catch (const std::bad_alloc&) {
        // An allocation failure is not a retryable condition; it stays a defect
        // (design §7) instead of being downgraded to a value.
        throw;
    } catch (const std::exception& e) {
        // Defence against unexpected exceptions only — expected failures are
        // error values handled above.
        YLOG_ERROR(services.logger, "Unhandled exception during update of {}. {}", task.fqdn, e.what());
        co_return std::unexpected(domain::UpdateError{domain::UpdateError::Code::UNKNOWN, e.what()});
    } catch (...) {
        YLOG_ERROR(services.logger, "Unknown non-standard exception during update for {}", task.fqdn);
        co_return std::unexpected(
            domain::UpdateError{domain::UpdateError::Code::UNKNOWN, "Unknown non-standard exception"});
    }
}

}  // namespace app
