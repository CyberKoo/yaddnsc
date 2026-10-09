//
// plugin — the per-call Host Services table over the coroutine bridge.
//

#ifndef YADDNSC_PLUGIN_HOST_SERVICES_H
#define YADDNSC_PLUGIN_HOST_SERVICES_H

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <stdint.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "infrastructure/plugin/bridge.h"

class Logger;

namespace plugin {

/// HostServicesContext — the per-update state behind yaddnsc_host_services,
/// re-expressed over the coroutine bridge.
///
/// One context is constructed on the offload worker for every ABI cycle and
/// bound to the Bridge and the logger. Cancellation is observed through the
/// call's shared CallState: the abandon latch the gateway sets, plus the
/// in-flight exchange's own flag. It owns the response arena: every string and
/// header array a plugin may borrow stays valid until the cycle returns, across
/// multiple exchanges (the ABI memory rules).
///
/// @note Single-threaded: one context serves exactly one ABI cycle, and the
///       whole cycle runs on one offload worker.
class HostServicesContext {
public:
    /// @param bridge Host-service HTTP bridge, owned by the gateway.
    /// @param logger Log port receiving plugin log records.
    /// @param state  Per-call cancellation state, shared with the gateway.
    HostServicesContext(Bridge& bridge, const Logger& logger, std::shared_ptr<CallState> state);

    /// Build the services table bound to this context. The returned table
    /// copies no state; it must not outlive the context.
    ///
    /// @param http_enabled When false, http_exchange fails with
    ///                     INVALID_ARGUMENT and does not touch the network.
    ///                     validate_config uses that table; update uses the
    ///                     real exchange.
    [[nodiscard]] yaddnsc_host_services make_services(bool http_enabled = true) noexcept {
        return yaddnsc_host_services{
            .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_host_services)),
            .abi_major = YADDNSC_DRIVER_ABI_MAJOR,
            .abi_minor = YADDNSC_DRIVER_ABI_MINOR,
            .context = this,
            .log = &log_entry,
            .http_exchange = http_enabled ? &http_exchange_entry : &http_exchange_unavailable_entry,
            .is_cancelled = &is_cancelled_entry,
        };
    }

    // Trampoline bodies (called through the C function pointers above).
    void log(yaddnsc_log_level level, yaddnsc_string message, const yaddnsc_source_location* location);
    yaddnsc_status http_exchange(const yaddnsc_http_request& request, yaddnsc_http_response* out_response,
                                 yaddnsc_error* out_error);

    /// 0 while this cycle is active; non-zero once it was cancelled (either the
    /// gateway abandoned it or the in-flight bridge exchange was cancelled).
    [[nodiscard]] std::int32_t is_cancelled() const noexcept {
        if (state_->cancelled.load(std::memory_order_acquire)) {
            return 1;
        }
        const std::shared_ptr<BridgeCall> call = state_->in_flight.load(std::memory_order_acquire);
        return call != nullptr && call->cancelled.load(std::memory_order_acquire) ? 1 : 0;
    }

private:
    /// Arena-owning copy of a string; the returned view stays valid until the
    /// context is destroyed.
    [[nodiscard]] std::string_view arena_copy(std::string_view value);

    static void log_entry(void* context, yaddnsc_log_level level, yaddnsc_string message,
                          const yaddnsc_source_location* location) noexcept;

    static yaddnsc_status http_exchange_entry(void* context, const yaddnsc_http_request* request,
                                              yaddnsc_http_response* out_response, yaddnsc_error* out_error);

    static yaddnsc_status http_exchange_unavailable_entry(void* context, const yaddnsc_http_request* request,
                                                          yaddnsc_http_response* out_response, yaddnsc_error* out_error);

    static std::int32_t is_cancelled_entry(void* context) noexcept {
        return static_cast<HostServicesContext*>(context)->is_cancelled();
    }

    Bridge& bridge_;
    const Logger& logger_;
    std::shared_ptr<CallState> state_;

    // Arenas — std::deque never invalidates references on push_back.
    std::deque<std::string> string_arena_;
    std::deque<std::vector<yaddnsc_http_header>> header_array_arena_;
};

}  // namespace plugin

#endif  // YADDNSC_PLUGIN_HOST_SERVICES_H
