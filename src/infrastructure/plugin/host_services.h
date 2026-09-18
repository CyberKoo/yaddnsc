//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_HOST_SERVICES_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_HOST_SERVICES_H

#include <stdint.h>
#include <yaddnsc/sdk/driver_abi.h>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "support/util/cancellation_token.hpp"

class HttpClient;
class Logger;

/// HostServicesContext — the per-update state behind yaddnsc_host_services.
///
/// One context is constructed on the stack for every update call and bound
/// to a fresh HttpClient and the logger. The host-only HTTP cancellation
/// token is never exposed through the plugin ABI. It owns the response arena:
/// every string and header array a plugin
/// may borrow stays valid until the update call returns, across multiple
/// exchanges (the ABI memory rules).
///
/// @note Single-threaded: one context serves exactly one update call.
class HostServicesContext {
public:
    /// @param http_token  Host-only token passed directly to HttpClient by
    ///                    http_exchange(); plugins cannot observe it.
    HostServicesContext(HttpClient& http_client, const Logger& logger, Utils::CancellationToken http_token);

    /// Build the services table bound to this context. The returned table
    /// copies no state; it must not outlive the context.
    [[nodiscard]] yaddnsc_host_services make_services() noexcept {
        return yaddnsc_host_services{
            .struct_size = static_cast<uint32_t>(sizeof(yaddnsc_host_services)),
            .api_revision = YADDNSC_DRIVER_API_REVISION,
            .context = this,
            .log = &log_entry,
            .http_exchange = &http_exchange_entry,
            .is_cancelled = &is_cancelled_entry,
        };
    }

    // Trampoline bodies (called through the C function pointers above).
    void log(yaddnsc_log_level level, yaddnsc_string message, const yaddnsc_source_location* location);
    yaddnsc_status http_exchange(const yaddnsc_http_request& request,
                                 yaddnsc_http_response* out_response,
                                 yaddnsc_error* out_error);

    /// Plugin cancellation is deliberately independent of host lifecycle
    /// cancellation. Host-side cancellation only aborts individual HTTP I/O.
    [[nodiscard]] int is_cancelled() const noexcept { return 0; }

private:
    /// Arena-owning copy of a string; the returned view stays valid until the
    /// context is destroyed.
    [[nodiscard]] std::string_view arena_copy(std::string_view value);

    static void log_entry(void* context,
                          yaddnsc_log_level level,
                          yaddnsc_string message,
                          const yaddnsc_source_location* location) noexcept {
        // Contract: logging failure must never fail an update — and a host
        // exception (e.g. bad_alloc) must never escape into the plugin's
        // C frame.
        try {
            static_cast<HostServicesContext*>(context)->log(level, message, location);
        } catch (...) {
        }
    }

    static yaddnsc_status http_exchange_entry(void* context,
                                              const yaddnsc_http_request* request,
                                              yaddnsc_http_response* out_response,
                                              yaddnsc_error* out_error);

    static int is_cancelled_entry(void* context) noexcept {
        return static_cast<HostServicesContext*>(context)->is_cancelled();
    }

    HttpClient& http_client_;
    const Logger& logger_;
    Utils::CancellationToken http_token_;

    // Arenas — std::deque never invalidates references on push_back.
    std::deque<std::string> string_arena_;
    std::deque<std::vector<yaddnsc_http_header>> header_array_arena_;
};

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_HOST_SERVICES_H
