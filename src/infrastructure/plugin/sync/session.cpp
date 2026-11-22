#include "session.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/nursery.hpp"
#include "coro/offload.hpp"
#include "coro/scope.hpp"
#include "coro/with_nursery.hpp"
#include "domain/dns/record_kind.h"
#include "domain/error/error.h"
#include "infrastructure/plugin/detail/abi_result.h"
#include "infrastructure/plugin/detail/abi_string.h"
#include "infrastructure/plugin/sync/driver_instance.h"
#include "infrastructure/plugin/sync/host_services.h"

namespace plugin {
namespace {

struct DestroyPhase {
    HostServicesContext& context;

    ~DestroyPhase() noexcept { context.set_phase(HostServicesContext::Phase::DESTROY); }
};

template<typename MapFailure>
std::expected<yaddnsc_driver*, domain::DriverError> create_driver(const SyncPluginPackage& package,
                                                                  const yaddnsc_host_services& services,
                                                                  MapFailure map_failure) {
    yaddnsc_error error{};
    error.struct_size = static_cast<std::uint32_t>(sizeof(error));
    yaddnsc_driver* handle = nullptr;
    const auto status = package.create(services, &handle, error);
    if (status == YADDNSC_STATUS_OK && handle != nullptr) {
        return handle;
    }
    if (status == YADDNSC_STATUS_OK || !detail::has_valid_plugin_error(error, status)) {
        return std::unexpected(detail::invalid_plugin_result(package.descriptor().name, "create"));
    }
    return std::unexpected(map_failure(status, detail::to_view(error.message), error.retry_after_seconds));
}

}  // namespace

void SyncDriverSession::BridgeState::register_call(const std::shared_ptr<BridgeCall>& call) {
    calls.push_back(call);
}

void SyncDriverSession::BridgeState::unregister_call(const std::shared_ptr<BridgeCall>& call) noexcept {
    std::erase_if(calls, [&call](const auto& tracked) { return tracked.lock() == call; });
}

SyncDriverSession::SyncDriverSession(std::shared_ptr<const SyncPluginPackage> package,
                                     domain::DriverUpdateCommand command, SessionDependencies dependencies)
    : package_(std::move(package)), dependencies_(std::move(dependencies)),
      bridge_(std::make_shared<Bridge>(dependencies_.loop, dependencies_.cycle_nursery, dependencies_.http,
                                       dependencies_.exchange_budget)),
      state_(std::make_shared<BridgeState>()), command_(std::move(command)), operation_(Operation::UPDATE) {}

SyncDriverSession::SyncDriverSession(std::shared_ptr<const SyncPluginPackage> package, std::string driver_param_json,
                                     SessionDependencies dependencies)
    : package_(std::move(package)), dependencies_(std::move(dependencies)),
      bridge_(std::make_shared<Bridge>(dependencies_.loop, dependencies_.cycle_nursery, dependencies_.http,
                                       dependencies_.exchange_budget)),
      state_(std::make_shared<BridgeState>()), driver_param_json_(std::move(driver_param_json)),
      operation_(Operation::VALIDATE) {}

void SyncDriverSession::request_cancel() noexcept {
    state_->cancelled_flag.store(true, std::memory_order_release);
    if (state_->worker_scope != nullptr) {
        state_->worker_scope->cancel();
    }
    cancel_calls();
}

void SyncDriverSession::cancel_calls() noexcept {
    for (const auto& tracked : state_->calls) {
        if (const auto call = tracked.lock()) {
            call->cancelled.store(true, std::memory_order_release);
            if (call->live_scope != nullptr) {
                call->live_scope->cancel();
            }
        }
    }
}

coro::Task<DriverSessionOutcome> SyncDriverSession::run() {
    if (state_->cancelled()) {
        co_return DriverSessionOutcome{{}, true};
    }

    DriverSessionResult result;
    bool plugin_cancelled = false;
    try {
        co_await coro::with_nursery(
            [this, &result, &plugin_cancelled](coro::Nursery& group) -> coro::Task<void> {
                state_->lifetime = coro::nursery_token(group);
                auto outcome = co_await coro::with_cancel_scope(
                    [this](coro::CancelScope& scope) -> coro::Task<DriverSessionResult> {
                        struct WorkerScopeGuard {
                            BridgeState& state;
                            ~WorkerScopeGuard() noexcept { state.worker_scope = nullptr; }
                        } guard{*state_};
                        state_->worker_scope = &scope;
                        if (state_->cancelled()) {
                            scope.cancel();
                        }
                        co_return co_await coro::offload_joined([this] { return run_operation(); });
                    });
                if (outcome.cancelled) {
                    plugin_cancelled = true;
                } else {
                    result = std::move(*outcome);
                }
                if (state_->cancelled()) {
                    cancel_calls();
                }
            },
            coro::NurseryPolicy::SUPERVISOR);
    } catch (const detail::AbiCancelled&) {
        plugin_cancelled = true;
        cancel_calls();
    } catch (const coro::Cancelled&) {
        state_->cancelled_flag.store(true, std::memory_order_release);
        plugin_cancelled = true;
        cancel_calls();
    }

    co_return DriverSessionOutcome{std::move(result), plugin_cancelled};
}

coro::Task<void> SyncDriverSession::drain() {
    co_return;
}

DriverSessionResult SyncDriverSession::run_operation() {
    if (operation_ == Operation::UPDATE) {
        return run_update();
    }
    return run_validation();
}

DriverSessionResult SyncDriverSession::run_update() {
    const auto& command = *command_;
    const std::string_view driver_name = package_->descriptor().name;
    if (state_->cancelled()) {
        throw detail::AbiCancelled{};
    }

    HostServicesContext context{*bridge_, dependencies_.logger, state_};
    const auto services = context.make_services_v1_1();
    const auto created = create_driver(
        *package_, services.base,
        [&](const yaddnsc_status status, const std::string_view message, const std::uint32_t retry_after_seconds) {
            return detail::map_error(status, message, driver_name, command.fqdn, retry_after_seconds);
        });
    if (!created) {
        return std::unexpected(created.error());
    }

    DriverInstance instance{package_, *created};
    const DestroyPhase destroy_phase{context};
    const std::string ip_address = command.ip_address.to_string();
    const auto record_kind = domain::record_kind_to_str(command.record_kind);
    const yaddnsc_update_request request{
        .struct_size = static_cast<std::uint32_t>(sizeof(request)),
        .ip_address = {ip_address.data(), ip_address.size()},
        .record_type = {record_kind.data(), record_kind.size()},
        .domain = {command.domain.data(), command.domain.size()},
        .subdomain = {command.subdomain.data(), command.subdomain.size()},
        .fqdn = {command.fqdn.data(), command.fqdn.size()},
        .driver_param_json = {reinterpret_cast<const std::uint8_t*>(command.driver_params.data()),
                              command.driver_params.size()},
    };
    yaddnsc_error error{};
    error.struct_size = static_cast<std::uint32_t>(sizeof(error));
    context.set_phase(HostServicesContext::Phase::UPDATE);
    const auto status = instance.update(request, error);
    context.set_phase(HostServicesContext::Phase::DESTROY);
    context.rethrow_defect();
    if (status == YADDNSC_STATUS_OK) {
        return {};
    }
    if (!detail::has_valid_plugin_error(error, status)) {
        return std::unexpected(detail::invalid_plugin_result(driver_name, "update"));
    }
    return std::unexpected(detail::map_error(status, detail::to_view(error.message), driver_name, command.fqdn,
                                             error.retry_after_seconds));
}

DriverSessionResult SyncDriverSession::run_validation() {
    const std::string_view driver_name = package_->descriptor().name;
    if (state_->cancelled()) {
        throw detail::AbiCancelled{};
    }

    HostServicesContext context{*bridge_, dependencies_.logger, state_};
    const auto services = context.make_services_v1_1();
    const auto created =
        create_driver(*package_, services.base,
                      [&](const yaddnsc_status status, const std::string_view message, const std::uint32_t) {
                          return detail::map_validate_error(status, message, driver_name);
                      });
    if (!created) {
        return std::unexpected(created.error());
    }

    DriverInstance instance{package_, *created};
    const DestroyPhase destroy_phase{context};
    yaddnsc_error error{};
    error.struct_size = static_cast<std::uint32_t>(sizeof(error));
    const yaddnsc_string param{driver_param_json_.data(), driver_param_json_.size()};
    context.set_phase(HostServicesContext::Phase::VALIDATE);
    const auto status = instance.validate(param, error);
    context.set_phase(HostServicesContext::Phase::DESTROY);
    context.rethrow_defect();
    if (status == YADDNSC_STATUS_OK) {
        return {};
    }
    if (!detail::has_valid_plugin_error(error, status)) {
        return std::unexpected(detail::invalid_plugin_result(driver_name, "validate"));
    }
    return std::unexpected(detail::map_validate_error(status, detail::to_view(error.message), driver_name));
}

std::unique_ptr<DriverSession> SyncPluginPackage::create_update_session(domain::DriverUpdateCommand command,
                                                                        SessionDependencies dependencies) const {
    auto self = std::static_pointer_cast<const SyncPluginPackage>(shared_from_this());
    return std::make_unique<SyncDriverSession>(std::move(self), std::move(command), std::move(dependencies));
}

std::unique_ptr<DriverSession> SyncPluginPackage::create_validation_session(std::string driver_param_json,
                                                                            SessionDependencies dependencies) const {
    auto self = std::static_pointer_cast<const SyncPluginPackage>(shared_from_this());
    return std::make_unique<SyncDriverSession>(std::move(self), std::move(driver_param_json), std::move(dependencies));
}

}  // namespace plugin
