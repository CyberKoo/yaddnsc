#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_HTTP_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_HTTP_H

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/task.hpp"
#include "infrastructure/http/types.h"
#include "infrastructure/plugin/async/services.h"

namespace plugin {

/// Owned storage for one ABI event delivered to the async protocol session.
struct AsyncEventPacket {
    yaddnsc_event event{};
    yaddnsc_http_response response{};
    yaddnsc_error error{};
    std::vector<std::pair<std::string, std::string>> header_storage;
    std::vector<yaddnsc_http_header> headers;
    std::string body;
    std::string error_message;

    AsyncEventPacket();
};

struct AsyncHttpCompletion {
    std::uint64_t op_id{0};
    std::shared_ptr<AsyncEventPacket> packet;
    std::exception_ptr defect;
};

/// Actor owning every async HTTP operation, cancellation scope and completion
/// queue entry. ABI protocol code submits commands and consumes events; HTTP
/// tasks only publish immutable completions back to this actor.
class AsyncHttpPump : public std::enable_shared_from_this<AsyncHttpPump> {
public:
    struct ApplyResult {
        std::vector<std::uint64_t> discarded;
        bool idle{true};
    };

    AsyncHttpPump(http::Options options, std::chrono::milliseconds budget);

    [[nodiscard]] coro::Task<void> run();
    [[nodiscard]] coro::Task<ApplyResult> apply(AsyncStepCommands commands, bool terminal);
    void cancel() noexcept;
    [[nodiscard]] coro::Task<void> finish();
    [[nodiscard]] coro::Task<std::optional<AsyncHttpCompletion>> next_completion();

    struct State;

private:
    std::shared_ptr<State> state_;
};

[[nodiscard]] std::shared_ptr<AsyncEventPacket> make_cancel_event();

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_HTTP_H
