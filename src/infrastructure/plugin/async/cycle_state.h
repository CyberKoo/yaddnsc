#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_CYCLE_STATE_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_CYCLE_STATE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <yaddnsc/sdk/driver_abi.h>

#include "domain/update/driver_update_command.h"
#include "infrastructure/http/types.h"
#include "infrastructure/plugin/async/services.h"
#include "infrastructure/plugin/detail/cycle.h"

class PluginModule;

namespace app {
class LoggerPort;
}

namespace plugin::detail {

/// Stable owning ABI event storage. Published on the loop, borrowed by exactly
/// one joined resume step; the packet survives until that entry returns.
struct AsyncEventPacket {
    yaddnsc_event event{};
    yaddnsc_http_response response{};
    yaddnsc_error error{};
    std::vector<std::pair<std::string, std::string>> header_storage;
    std::vector<yaddnsc_http_header> headers;
    std::string body;
    std::string error_message;

    AsyncEventPacket() {
        event.struct_size = static_cast<uint32_t>(sizeof(event));
        event.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
        event.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
    }
};

/// Loop-only HTTP registration. scope is borrowed only while the task runs;
/// ready prevents late cancellation from overwriting an already queued result.
struct AsyncOp {
    uint64_t id = 0;
    std::string url;
    http::Request request;
    coro::CancelScope* scope = nullptr;
    bool ready = false;
    bool canceled = false;
};

/// Runner-owned per-update resources, shared with all live/retiring HTTP tasks.
/// Handle/services/commands hand off to one joined worker step at a time; only
/// CallState::cancelled is concurrently read. Completion queues and scopes are
/// loop-only. No borrowed command or service storage can outlive real drain.
struct AsyncResources {
    std::shared_ptr<CallState> state;
    std::shared_ptr<const PluginModule> module;
    domain::DriverUpdateCommand command;
    const app::LoggerPort* logger;
    http::Options http;
    std::chrono::milliseconds budget;
    std::unique_ptr<HostAsyncServicesContext> services;
    AsyncStepCommands commands;
    std::deque<std::shared_ptr<AsyncEventPacket>> completions;
    std::map<uint64_t, std::shared_ptr<AsyncOp>> ops;
    std::size_t active_tasks = 0;
    bool stopping = false;
    yaddnsc_async_driver* handle = nullptr;

    AsyncResources(std::shared_ptr<CallState> call_state, std::shared_ptr<const PluginModule> driver_module,
                   domain::DriverUpdateCommand update_command, const app::LoggerPort& log_port, http::Options options,
                   std::chrono::milliseconds exchange_budget)
        : state(std::move(call_state)), module(std::move(driver_module)), command(std::move(update_command)),
          logger(&log_port), http(std::move(options)), budget(exchange_budget),
          services(std::make_unique<HostAsyncServicesContext>(log_port, state)) {}
};

using AsyncResourcePtr = std::shared_ptr<AsyncResources>;

}  // namespace plugin::detail

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_CYCLE_STATE_H
