//
// plugin — the per-cycle async Host Services table (ABI 1.1).
//

#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_SERVICES_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_SERVICES_H

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <yaddnsc/sdk/driver_abi.h>

#include "infrastructure/http/types.h"
#include "infrastructure/plugin/detail/cycle.h"

namespace app {
class LoggerPort;
}

namespace plugin {

/// One accepted HTTP command. The worker copies every borrowed view before
/// returning; the loop starts the request only after the step has returned.
struct AsyncStartCommand {
    uint64_t op_id = 0;
    std::string url;
    http::Request request;
};

/// Commands collected during one plugin step. Worker-only while the step runs,
/// loop-only after the offload join hands the result back.
struct AsyncStepCommands {
    std::vector<AsyncStartCommand> starts;
    std::vector<uint64_t> cancels;

    void clear() {
        starts.clear();
        cancels.clear();
    }
};

/// HostAsyncServicesContext — the per-cycle state behind yaddnsc_async_services.
///
/// The table and context live from async_create through async_destroy. The
/// worker sets the phase and the step command sink around each entry; the loop
/// applies the collected commands after the step returns. HTTP callbacks never
/// start an exchange inline.
class HostAsyncServicesContext {
public:
    enum class Phase { CREATE, BEGIN, RESUME, VALIDATE, DESTROY };

    HostAsyncServicesContext(const app::LoggerPort& logger, std::shared_ptr<CallState> state);

    HostAsyncServicesContext(const HostAsyncServicesContext&) = delete;
    HostAsyncServicesContext& operator=(const HostAsyncServicesContext&) = delete;

    /// The stable table passed to async_create. It lives as long as this
    /// context, which the cycle keeps through async_destroy.
    [[nodiscard]] const yaddnsc_async_services& services() const noexcept { return services_; }

    void set_phase(Phase phase) noexcept { phase_ = phase; }

    void set_commands(AsyncStepCommands* commands) noexcept { commands_ = commands; }

    void clear_commands() noexcept { commands_ = nullptr; }

    /// Loop-only, between steps: the op has been delivered or discarded.
    void mark_delivered(uint64_t op_id) noexcept { active_ops_.erase(op_id); }

    [[nodiscard]] std::int32_t is_cancelled() const noexcept;

private:
    void log(yaddnsc_log_level level, yaddnsc_string message, const yaddnsc_source_location* location);
    std::int32_t log_enabled(yaddnsc_log_level level) const noexcept;
    yaddnsc_status http_start(const yaddnsc_http_request& request, uint64_t* out_op_id, yaddnsc_error* out_error);
    yaddnsc_status http_cancel(uint64_t op_id, yaddnsc_error* out_error);

    static void log_entry(void* context, yaddnsc_log_level level, yaddnsc_string message,
                          const yaddnsc_source_location* location) noexcept;
    static std::int32_t log_enabled_entry(void* context, yaddnsc_log_level level) noexcept;
    static std::int32_t is_cancelled_entry(void* context) noexcept;
    static yaddnsc_status http_start_entry(void* context, const yaddnsc_http_request* request, uint64_t* out_op_id,
                                           yaddnsc_error* out_error) noexcept;
    static yaddnsc_status http_cancel_entry(void* context, uint64_t op_id, yaddnsc_error* out_error) noexcept;

    const app::LoggerPort& logger_;
    std::shared_ptr<CallState> state_;
    yaddnsc_async_services services_{};
    Phase phase_ = Phase::CREATE;
    AsyncStepCommands* commands_ = nullptr;
    uint64_t next_op_id_ = 1;
    /// Accepted but not yet delivered ops. Worker during a step, loop between
    /// steps; the offload join provides the handoff.
    std::set<uint64_t> active_ops_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_SERVICES_H
