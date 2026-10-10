//
// app — one coroutine update cycle (implementation).
//

#include "run_update_cycle.h"

#include <magic_enum/magic_enum.hpp>
#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <chrono>
#include <expected>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "application/log.h"
#include "domain/address_policy.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "infrastructure/coro/cancelled.h"
#include "infrastructure/coro/scope.hpp"
#include "support/fmt.hpp"
#include "application/ports/gateway.h"
#include "application/ports/ip_source.h"
#include "application/ports/resolver.h"
#include "application/services.h"
#include "domain/config/runtime_config.h"
#include "domain/update/driver_update_command.h"
#include "domain/update/update_decision.h"
#include "domain/update/update_task.h"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace app {

coro::Task<UpdateCycleOutcome> run_update_cycle(const domain::UpdateTask& task, const Services& services) {
    const auto& subdomain = task.subdomain;
    const auto rd_type_name = magic_enum::enum_name(subdomain.type);
    const std::string_view rd_type = rd_type_name.empty() ? std::string_view{"UNKNOWN"} : rd_type_name;

    try {
        // --- Step 1: local IP -------------------------------------------------
        // The source gets its own budget so a stalled provider (a peer that
        // accepts and never answers) fails this cycle in seconds instead of
        // holding it to UPDATE_BUDGET — the coroutine HTTP layer has no I/O
        // timeout, so the bound is composed here like the DNS read below.

        const auto source_result = co_await coro::with_timeout(
            IP_SOURCE_BUDGET,
            [&services,
             &subdomain]() -> coro::Task<std::expected<std::vector<domain::InetAddress>, domain::IpSourceError>> {
                co_return co_await services.ip_source.resolve(subdomain);
            });
        if (source_result.timed_out) {
            // Our own budget fired: a transient source
            // failure, not an abort — skip this cycle.
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(IP_SOURCE_BUDGET).count();
            YLOG_ERROR(services.logger, "Failed to resolve local IP address for {}, skipping the update: {}", task.fqdn,
                       fmt::format("timed out after {}s", seconds));
            co_return std::unexpected(domain::UpdateError{domain::UpdateError::Code::SKIPPED_NO_ADDRESS,
                                                          fmt::format("IP source timed out after {}s", seconds)});
        }

        const auto& candidates = *source_result;
        if (!candidates) {
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
                [&services, &task,
                 &subdomain]() -> coro::Task<std::expected<std::vector<std::string>, domain::DnsErrorInfo>> {
                    co_return co_await services.resolver.resolve(task.fqdn, subdomain.type);
                });

            if (read.timed_out) {
                // Our own budget fired: a transient read failure, not an abort —
                // proceed with the update on an empty record list.
                YLOG_DEBUG(services.logger,
                           R"(DNS lookup for "{}" failed: timed out after {}s, proceeding with update)", task.fqdn,
                           std::chrono::duration_cast<std::chrono::seconds>(DNS_READ_BUDGET).count());
            } else if (read.has_value()) {
                const auto& resolved = *read;
                if (!resolved) {
                    // Cannot verify the current record (transient failure, NXDOMAIN,
                    // NODATA, ...) — proceed with the update anyway: pushing an
                    // unchanged record is harmless, while skipping a changed one is not.
                    YLOG_DEBUG(services.logger, R"(DNS lookup for "{}" failed: {} ({}), proceeding with update)",
                               task.fqdn, resolved.error().message, domain::error_to_str(resolved.error().code));
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
            .domain = task.domain.name,
            .subdomain = subdomain.name,
            .fqdn = task.fqdn,
        };

        const auto result = co_await services.gateway.update(task.domain.driver, command);
        if (!result) {
            switch (result.error().code) {
                case domain::DriverError::Code::NOT_FOUND:
                    YLOG_ERROR(services.logger, "Driver '{}' not found for task '{}', skipping: {}", task.domain.driver,
                               task.fqdn, result.error().message);
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
                                                          result.error().message, result.error().retry_after_seconds});
        }

        YLOG_INFO(services.logger, "Domain {} ({}) updated to {}", task.fqdn, rd_type, local_addr);
        co_return UpdateCycleResult{decision};
    } catch (const std::bad_alloc&) {
        // An allocation failure is not a retryable condition; it stays a defect
        // (design §7) instead of being downgraded to a value.
        throw;
    } catch (const coro::Cancelled&) {
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
