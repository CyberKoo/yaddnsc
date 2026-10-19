//
// app — the pre-start driver configuration gate (implementation).
//

#include "config_validation.h"

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <chrono>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "application/ports/gateway.h"
#include "coro/scope.hpp"
#include "domain/config/runtime_config.h"
#include "domain/fqdn.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace app {

coro::Task<ConfigValidationOutcome> validate_driver_configs(GatewayPort& gateway, const domain::RuntimeConfig& config,
                                                            ConfigValidationMode mode, coro::Duration budget) {
    ConfigValidationOutcome outcome;
    const auto budget_seconds = std::chrono::duration_cast<std::chrono::seconds>(budget).count();
    // An identical (driver, driver_params) pair validates once: the plugin's
    // answer cannot differ between records, and a slow plugin would otherwise
    // multiply startup by the duplicate count. Views borrow the configuration,
    // which outlives this sweep. Only passes are cached — a rejection or an
    // expired budget stops the sweep on the spot.
    std::set<std::pair<std::string_view, std::string_view>> validated_pairs;
    for (const auto& domain_config : config.domains) {
        const auto can_check = gateway.supports_validate(domain_config.driver);
        for (const auto& subdomain : domain_config.subdomains) {
            if (!can_check) {
                if (mode == ConfigValidationMode::REQUIRE_VALIDATION) {
                    // This command exists to answer "is this configuration
                    // good?"; an unchecked driver is not an answer.
                    outcome.rejection =
                        fmt::format("Driver '{}' does not provide yaddnsc_driver_validate; its configuration was not checked",
                                    domain_config.driver);
                    co_return outcome;
                }
                if (std::ranges::find(outcome.unchecked_drivers, domain_config.driver) ==
                    outcome.unchecked_drivers.end()) {
                    outcome.unchecked_drivers.push_back(domain_config.driver);
                }
                continue;
            }

            if (!validated_pairs.emplace(domain_config.driver, subdomain.driver_params).second) {
                continue;
            }
            const auto fqdn = domain::make_fqdn(domain_config.name, subdomain.name);
            const auto checked = co_await coro::with_timeout(budget, [&gateway, &domain_config, &subdomain] {
                return gateway.validate_config(domain_config.driver, subdomain.driver_params);
            });
            if (checked.timed_out) {
                outcome.rejection = fmt::format("Driver '{}' did not finish validating the configuration for {} within {}s",
                                                domain_config.driver, fqdn, budget_seconds);
                co_return outcome;
            }
            if (!*checked) {
                outcome.rejection =
                    fmt::format("Driver '{}' rejected configuration for {}: {}", domain_config.driver, fqdn,
                                checked->error().message);
                co_return outcome;
            }
        }
    }
    co_return outcome;
}

}  // namespace app
