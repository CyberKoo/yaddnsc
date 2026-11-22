#include "session.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/nursery.hpp"
#include "coro/with_nursery.hpp"
#include "domain/dns/record_kind.h"
#include "infrastructure/plugin/detail/abi_result.h"

namespace plugin {

AsyncDriverSession::AsyncDriverSession(std::shared_ptr<const AsyncPluginPackage> package,
                                       domain::DriverUpdateCommand command, SessionDependencies dependencies)
    : package_(std::move(package)), dependencies_(std::move(dependencies)),
      cancellation_(std::make_shared<Cancellation>()),
      pump_(std::make_shared<AsyncHttpPump>(dependencies_.http, dependencies_.exchange_budget)),
      services_(dependencies_.logger, cancellation_), command_(std::move(command)), operation_(Operation::UPDATE) {}

AsyncDriverSession::AsyncDriverSession(std::shared_ptr<const AsyncPluginPackage> package, std::string driver_param_json,
                                       SessionDependencies dependencies)
    : package_(std::move(package)), dependencies_(std::move(dependencies)),
      cancellation_(std::make_shared<Cancellation>()),
      pump_(std::make_shared<AsyncHttpPump>(dependencies_.http, dependencies_.exchange_budget)),
      services_(dependencies_.logger, cancellation_), driver_param_json_(std::move(driver_param_json)),
      operation_(Operation::VALIDATE) {}

void AsyncDriverSession::request_cancel() noexcept {
    cancellation_->flag.store(true, std::memory_order_release);
    pump_->cancel();
}

coro::Task<DriverSessionOutcome> AsyncDriverSession::run() {
    if (cancellation_->cancelled()) {
        co_return DriverSessionOutcome{{}, true};
    }
    if (operation_ == Operation::UPDATE) {
        co_return co_await run_update();
    }
    co_return co_await run_validation();
}

coro::Task<void> AsyncDriverSession::drain() {
    co_return;
}

bool AsyncDriverSession::finish_step(const detail::AsyncStepOutcome& outcome, const std::string_view entry,
                                     DriverSessionResult& result, bool& plugin_cancelled,
                                     const std::optional<std::string_view> fqdn) {
    const auto& driver_name = package_->descriptor().name;
    if (cancellation_->cancelled()) {
        plugin_cancelled = true;
        return false;
    }
    if (outcome.status == YADDNSC_STATUS_PENDING || !yaddnsc_async_step_status_is_valid(outcome.status)) {
        result = std::unexpected(detail::invalid_plugin_result(driver_name, entry));
        return false;
    }
    if (outcome.status == YADDNSC_STATUS_OK) {
        return true;
    }
    if (outcome.status == YADDNSC_STATUS_CANCELLED) {
        plugin_cancelled = true;
        return false;
    }
    if (!outcome.error_valid) {
        result = std::unexpected(detail::invalid_plugin_result(driver_name, entry));
        return false;
    }
    result = std::unexpected(
        fqdn ? detail::map_error(outcome.status, outcome.message, driver_name, *fqdn, outcome.retry_after_seconds)
             : detail::map_validate_error(outcome.status, outcome.message, driver_name));
    return false;
}

coro::Task<bool> AsyncDriverSession::apply_commands(const bool terminal) {
    auto applied = co_await pump_->apply(std::move(commands_), terminal);
    for (const std::uint64_t op_id : applied.discarded) {
        services_.mark_delivered(op_id);
    }
    co_return applied.idle;
}

detail::AsyncStepOutcome AsyncDriverSession::create_step() {
    yaddnsc_error error{};
    error.struct_size = static_cast<std::uint32_t>(sizeof(error));
    commands_.clear();
    services_.set_phase(HostAsyncServicesContext::Phase::CREATE);
    services_.set_commands(&commands_);
    yaddnsc_async_driver* handle = nullptr;
    const auto status = package_->async_create(services_.services(), &handle, error);
    services_.clear_commands();
    handle_ = status == YADDNSC_STATUS_OK ? handle : nullptr;
    return detail::make_step_outcome(status, error);
}

detail::AsyncStepOutcome AsyncDriverSession::begin_step() {
    const auto& command = *command_;
    const std::string ip_address = command.ip_address.to_string();
    const auto record_kind = domain::record_kind_to_str(command.record_kind);
    yaddnsc_error error{};
    error.struct_size = static_cast<std::uint32_t>(sizeof(error));
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
    commands_.clear();
    services_.set_phase(HostAsyncServicesContext::Phase::BEGIN);
    services_.set_commands(&commands_);
    const auto status = package_->async_begin(handle_, request, error);
    services_.clear_commands();
    return detail::make_step_outcome(status, error);
}

detail::AsyncStepOutcome AsyncDriverSession::resume_step(const std::shared_ptr<AsyncEventPacket>& event) {
    yaddnsc_error error{};
    error.struct_size = static_cast<std::uint32_t>(sizeof(error));
    commands_.clear();
    services_.set_phase(HostAsyncServicesContext::Phase::RESUME);
    services_.set_commands(&commands_);
    const auto status = package_->async_resume(handle_, event->event, error);
    services_.clear_commands();
    return detail::make_step_outcome(status, error);
}

detail::AsyncStepOutcome AsyncDriverSession::destroy_step() {
    services_.set_phase(HostAsyncServicesContext::Phase::DESTROY);
    package_->async_destroy(handle_);
    handle_ = nullptr;
    return detail::make_step_outcome(YADDNSC_STATUS_OK, yaddnsc_error{});
}

coro::Task<DriverSessionOutcome> AsyncDriverSession::execute_update() {
    DriverSessionResult result;
    bool plugin_cancelled = false;
    const std::string_view fqdn = command_->fqdn;

    const auto create = co_await detail::run_async_step([this] { return create_step(); });
    if (!finish_step(create, "async create", result, plugin_cancelled, fqdn)) {
        co_return DriverSessionOutcome{std::move(result), plugin_cancelled};
    }
    if (cancellation_->cancelled()) {
        co_return DriverSessionOutcome{{}, true};
    }

    const auto begin = co_await detail::run_async_step([this] { return begin_step(); });
    if (begin.status != YADDNSC_STATUS_PENDING) {
        co_await apply_commands(true);
        [[maybe_unused]] const bool begin_finished = finish_step(begin, "async begin", result, plugin_cancelled, fqdn);
        co_return DriverSessionOutcome{std::move(result), plugin_cancelled};
    }

    if (co_await apply_commands(false)) {
        result = std::unexpected(detail::invalid_plugin_result(package_->descriptor().name, "async begin"));
        co_return DriverSessionOutcome{std::move(result), false};
    }

    for (;;) {
        if (cancellation_->cancelled()) {
            auto cancel_event = make_cancel_event();
            const auto cancelled =
                co_await detail::run_async_step([this, cancel_event] { return resume_step(cancel_event); });
            if (cancelled.status == YADDNSC_STATUS_PENDING) {
                result = std::unexpected(detail::invalid_plugin_result(package_->descriptor().name, "async cancel"));
            } else {
                [[maybe_unused]] const bool cancel_finished =
                    finish_step(cancelled, "async cancel", result, plugin_cancelled, fqdn);
            }
            co_return DriverSessionOutcome{std::move(result), plugin_cancelled};
        }

        auto completion = co_await pump_->next_completion();
        if (cancellation_->cancelled()) {
            continue;
        }
        if (!completion.has_value()) {
            throw std::logic_error("async HTTP pump stopped without a completion");
        }
        if (completion->defect) {
            std::rethrow_exception(completion->defect);
        }

        auto event = std::move(completion->packet);
        if (event->event.kind == YADDNSC_EVENT_HTTP_FINISHED) {
            services_.mark_delivered(event->event.op_id);
        }
        const auto resumed = co_await detail::run_async_step([this, event] { return resume_step(event); });
        if (resumed.status == YADDNSC_STATUS_PENDING) {
            if (cancellation_->cancelled()) {
                continue;
            }
            if (co_await apply_commands(false)) {
                result = std::unexpected(detail::invalid_plugin_result(package_->descriptor().name, "async resume"));
                co_return DriverSessionOutcome{std::move(result), false};
            }
            continue;
        }

        co_await apply_commands(true);
        [[maybe_unused]] const bool resume_finished =
            finish_step(resumed, "async resume", result, plugin_cancelled, fqdn);
        co_return DriverSessionOutcome{std::move(result), plugin_cancelled};
    }
}

coro::Task<DriverSessionOutcome> AsyncDriverSession::run_update() {
    DriverSessionOutcome outcome;
    std::exception_ptr defect;
    co_await coro::with_nursery(
        [this, &outcome, &defect](coro::Nursery& group) -> coro::Task<void> {
            bool pump_started = false;
            try {
                group.spawn_discard(pump_->run());
                pump_started = true;
                outcome = co_await execute_update();
            } catch (...) {
                defect = std::current_exception();
            }
            if (pump_started) {
                try {
                    co_await pump_->finish();
                } catch (...) {
                    if (!defect) {
                        defect = std::current_exception();
                    }
                }
            }
        },
        coro::NurseryPolicy::SUPERVISOR);

    if (handle_ != nullptr) {
        co_await detail::run_async_destroy_step(package_->descriptor().name, "async destroy step did not complete",
                                                [this] { return destroy_step(); });
    }
    if (defect) {
        std::rethrow_exception(defect);
    }
    co_return outcome;
}

coro::Task<DriverSessionOutcome> AsyncDriverSession::run_validation() {
    DriverSessionOutcome outcome;
    std::exception_ptr defect;
    try {
        if (cancellation_->cancelled()) {
            outcome.plugin_cancelled = true;
        } else {
            const auto create = co_await detail::run_async_step([this] { return create_step(); });
            if (finish_step(create, "async create", outcome.result, outcome.plugin_cancelled)) {
                if (cancellation_->cancelled()) {
                    outcome.plugin_cancelled = true;
                } else {
                    const yaddnsc_string param{driver_param_json_.data(), driver_param_json_.size()};
                    const auto validated = co_await detail::run_async_step([this, param] {
                        yaddnsc_error error{};
                        error.struct_size = static_cast<std::uint32_t>(sizeof(error));
                        services_.set_phase(HostAsyncServicesContext::Phase::VALIDATE);
                        const auto status = package_->async_validate(handle_, param, error);
                        return detail::make_step_outcome(status, error);
                    });
                    [[maybe_unused]] const bool validation_finished =
                        finish_step(validated, "async validate", outcome.result, outcome.plugin_cancelled);
                }
            }
        }
    } catch (...) {
        defect = std::current_exception();
    }

    if (handle_ != nullptr) {
        co_await detail::run_async_destroy_step(package_->descriptor().name,
                                                "async validate destroy step did not complete",
                                                [this] { return destroy_step(); });
    }
    if (defect) {
        std::rethrow_exception(defect);
    }
    co_return outcome;
}

std::unique_ptr<DriverSession> AsyncPluginPackage::create_update_session(domain::DriverUpdateCommand command,
                                                                         SessionDependencies dependencies) const {
    auto self = std::static_pointer_cast<const AsyncPluginPackage>(shared_from_this());
    return std::make_unique<AsyncDriverSession>(std::move(self), std::move(command), std::move(dependencies));
}

std::unique_ptr<DriverSession> AsyncPluginPackage::create_validation_session(std::string driver_param_json,
                                                                             SessionDependencies dependencies) const {
    auto self = std::static_pointer_cast<const AsyncPluginPackage>(shared_from_this());
    return std::make_unique<AsyncDriverSession>(std::move(self), std::move(driver_param_json), std::move(dependencies));
}

}  // namespace plugin
