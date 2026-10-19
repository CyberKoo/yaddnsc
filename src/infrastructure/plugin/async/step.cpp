#include "step.h"

#include <atomic>
#include <expected>

#include "infrastructure/plugin/async/cycle_state.h"
#include "infrastructure/plugin/detail/abi_result.h"
#include "infrastructure/plugin/plugin_loader.h"

namespace plugin::detail {

AsyncStepOutcome make_step_outcome(yaddnsc_status status, const yaddnsc_error& error) {
    AsyncStepOutcome outcome;
    outcome.status = status;
    if (status == YADDNSC_STATUS_OK || status == YADDNSC_STATUS_PENDING) {
        return outcome;
    }
    if (has_valid_plugin_error(error, status)) {
        outcome.message.assign(error.message.data, error.message.size);
        outcome.retry_after_seconds = error.retry_after_seconds;
        outcome.error_valid = true;
    }
    return outcome;
}

bool finish_async_step(CallState& state, std::string_view driver_name, const AsyncStepOutcome& outcome,
                       std::string_view entry, std::optional<std::string_view> fqdn) {
    if (state.cancelled.load(std::memory_order_acquire)) {
        state.plugin_cancelled = true;
        return false;
    }
    if (outcome.status == YADDNSC_STATUS_PENDING || !yaddnsc_async_step_status_is_valid(outcome.status)) {
        state.result = std::unexpected(invalid_plugin_result(driver_name, entry));
        return false;
    }
    if (outcome.status == YADDNSC_STATUS_OK) {
        return true;
    }
    if (outcome.status == YADDNSC_STATUS_CANCELLED) {
        state.plugin_cancelled = true;
        return false;
    }
    if (!outcome.error_valid) {
        state.result = std::unexpected(invalid_plugin_result(driver_name, entry));
        return false;
    }
    state.result = std::unexpected(fqdn ? map_error(outcome.status, outcome.message, driver_name, *fqdn)
                                        : map_validate_error(outcome.status, outcome.message, driver_name));
    return false;
}

AsyncStepOutcome create_async_step(AsyncResources& res) {
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    res.commands.clear();
    res.services->set_phase(HostAsyncServicesContext::Phase::CREATE);
    res.services->set_commands(&res.commands);
    yaddnsc_async_driver* handle = nullptr;
    const auto status = res.module->async_create(res.services->services(), &handle, error);
    res.services->clear_commands();
    res.handle = status == YADDNSC_STATUS_OK ? handle : nullptr;
    return make_step_outcome(status, error);
}

AsyncStepOutcome begin_async_step(AsyncResources& res) {
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    const auto& params = res.command;
    const yaddnsc_update_request request{
        .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_update_request)),
        .ip_address = {params.ip_addr.data(), params.ip_addr.size()},
        .record_type = {params.rd_type.data(), params.rd_type.size()},
        .domain = {params.domain.data(), params.domain.size()},
        .subdomain = {params.subdomain.data(), params.subdomain.size()},
        .fqdn = {params.fqdn.data(), params.fqdn.size()},
        .driver_param_json = {reinterpret_cast<const uint8_t*>(params.driver_params.data()),
                              params.driver_params.size()},
    };
    res.commands.clear();
    res.services->set_phase(HostAsyncServicesContext::Phase::BEGIN);
    res.services->set_commands(&res.commands);
    const auto status = res.module->async_begin(res.handle, request, error);
    res.services->clear_commands();
    return make_step_outcome(status, error);
}

AsyncStepOutcome resume_async_step(AsyncResources& res, std::shared_ptr<AsyncEventPacket> event) {
    yaddnsc_error error{};
    error.struct_size = static_cast<uint32_t>(sizeof(error));
    res.commands.clear();
    res.services->set_phase(HostAsyncServicesContext::Phase::RESUME);
    res.services->set_commands(&res.commands);
    const auto status = res.module->async_resume(res.handle, event->event, error);
    res.services->clear_commands();
    return make_step_outcome(status, error);
}

AsyncStepOutcome destroy_async_step(AsyncResources& res) {
    res.services->set_phase(HostAsyncServicesContext::Phase::DESTROY);
    res.module->async_destroy(res.handle);
    res.handle = nullptr;
    return make_step_outcome(YADDNSC_STATUS_OK, yaddnsc_error{});
}

}  // namespace plugin::detail
