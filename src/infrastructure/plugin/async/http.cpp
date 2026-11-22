#include "http.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/notification.hpp"
#include "coro/now.hpp"
#include "coro/nursery.hpp"
#include "coro/scope.hpp"
#include "coro/with_nursery.hpp"
#include "infrastructure/http/client.h"
#include "infrastructure/http/error.h"
#include "infrastructure/http/types.h"

namespace plugin {
namespace {

struct AsyncOp {
    std::uint64_t id{0};
    std::string url;
    http::Request request;
    coro::CancelScope* scope{nullptr};
    bool ready{false};
    bool canceled{false};
};

struct ApplyWait {
    coro::Notification done;
    AsyncHttpPump::ApplyResult result;
    std::exception_ptr error;
};

struct Command {
    enum class Kind { APPLY, FINISH };

    Kind kind{Kind::APPLY};
    AsyncStepCommands commands{};
    bool terminal{false};
    std::shared_ptr<ApplyWait> wait{};
};

std::shared_ptr<AsyncEventPacket> make_http_event(const std::uint64_t op_id, const http::Response& response) {
    auto packet = std::make_shared<AsyncEventPacket>();
    packet->response.struct_size = static_cast<std::uint32_t>(sizeof(packet->response));
    packet->response.status_code = static_cast<std::uint32_t>(response.status_);
    packet->header_storage.reserve(response.headers_.size());
    packet->headers.reserve(response.headers_.size());
    for (const auto& [name, value] : response.headers_) {
        packet->header_storage.emplace_back(name, value);
    }
    for (const auto& [name, value] : packet->header_storage) {
        packet->headers.push_back({{name.data(), name.size()}, {value.data(), value.size()}});
    }
    packet->body = std::string(response.text());
    packet->response.headers = packet->headers.empty() ? nullptr : packet->headers.data();
    packet->response.header_count = packet->headers.size();
    packet->response.body = {reinterpret_cast<const std::uint8_t*>(packet->body.data()), packet->body.size()};
    packet->event.kind = YADDNSC_EVENT_HTTP_FINISHED;
    packet->event.op_id = op_id;
    packet->event.status = YADDNSC_STATUS_OK;
    packet->event.response = &packet->response;
    packet->event.error = nullptr;
    return packet;
}

std::shared_ptr<AsyncEventPacket> make_error_event(const std::uint64_t op_id, const yaddnsc_status status,
                                                   std::string message, const std::uint32_t retry_after_seconds) {
    auto packet = std::make_shared<AsyncEventPacket>();
    packet->error_message = std::move(message);
    packet->error.struct_size = static_cast<std::uint32_t>(sizeof(packet->error));
    packet->error.status = status;
    packet->error.retry_after_seconds = retry_after_seconds;
    packet->error.message = {packet->error_message.data(), packet->error_message.size()};
    packet->event.kind = YADDNSC_EVENT_HTTP_FINISHED;
    packet->event.op_id = op_id;
    packet->event.status = status;
    packet->event.response = nullptr;
    packet->event.error = &packet->error;
    return packet;
}

}  // namespace

struct AsyncHttpPump::State {
    State(http::Options client_options, const std::chrono::milliseconds exchange_budget)
        : options(std::move(client_options)), budget(exchange_budget) {}

    http::Options options;
    std::chrono::milliseconds budget;
    std::deque<Command> commands;
    std::deque<AsyncHttpCompletion> completions;
    std::map<std::uint64_t, std::shared_ptr<AsyncOp>> ops;
    coro::Notification changed;
    coro::Notification completion_changed;
    coro::Notification finished;
    std::size_t active_tasks{0};
    bool stopping{false};
    bool cancel_requested{false};
    bool finish_requested{false};
    bool done{false};
    std::exception_ptr error;
};

namespace {

using PumpState = std::shared_ptr<AsyncHttpPump::State>;

void record_error(PumpState& state, std::exception_ptr error) noexcept {
    if (!state->error) {
        state->error = std::move(error);
    }
}

void complete_http(PumpState state, const std::shared_ptr<AsyncOp>& op, AsyncHttpCompletion completion) noexcept {
    if (!op->ready) {
        op->ready = true;
        if (state->active_tasks != 0) {
            --state->active_tasks;
        }
        state->ops.erase(op->id);
    }
    try {
        state->completions.push_back(std::move(completion));
    } catch (...) {
        record_error(state, std::current_exception());
    }
    state->completion_changed.notify();
    state->changed.notify();
}

struct CompletionGuard {
    PumpState state;
    std::shared_ptr<AsyncOp> op;
    AsyncHttpCompletion completion{};

    ~CompletionGuard() noexcept { complete_http(std::move(state), op, std::move(completion)); }
};

coro::Task<void> run_http(PumpState state, std::shared_ptr<AsyncOp> op) {
    CompletionGuard guard{state, op};
    guard.completion.op_id = op->id;
    try {
        if (state->stopping || op->canceled || state->cancel_requested) {
            guard.completion.packet =
                make_error_event(op->id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0);
            co_return;
        }

        auto scoped = co_await coro::with_cancel_scope(
            [&state, &op](coro::CancelScope& scope) -> coro::Task<std::expected<http::Response, http::Error>> {
                op->scope = &scope;
                struct ScopeBorrow {
                    AsyncOp& op;
                    ~ScopeBorrow() noexcept { op.scope = nullptr; }
                } borrow{*op};
                if (state->stopping || op->canceled || state->cancel_requested) {
                    scope.cancel();
                }
                scope.throw_if_cancelled();
                const auto deadline = co_await coro::current_time() + state->budget;
                auto outcome = co_await coro::with_deadline(
                    deadline, [&state, &op]() -> coro::Task<std::expected<http::Response, http::Error>> {
                        http::Client client{state->options};
                        co_return co_await client.exchange(op->url, op->request);
                    });
                if (outcome.timed_out) {
                    co_return std::unexpected(
                        http::Error{http::ErrorCode::QUEUE_TIMEOUT, "the API server did not respond in time", 0});
                }
                if (outcome.cancelled) {
                    co_return std::unexpected(
                        http::Error{http::ErrorCode::CONNECTION_LOST, "the request was cancelled", 0});
                }
                co_return std::move(*outcome);
            });

        if (scoped.cancelled) {
            guard.completion.packet =
                make_error_event(op->id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0);
        } else if (scoped.timed_out) {
            guard.completion.packet =
                make_error_event(op->id, YADDNSC_STATUS_NETWORK_ERROR, "the API server did not respond in time", 0);
        } else if (!scoped.has_value()) {
            guard.completion.packet =
                make_error_event(op->id, YADDNSC_STATUS_INTERNAL_ERROR, "async HTTP task produced no result", 0);
        } else if (*scoped) {
            guard.completion.op_id = op->id;
            guard.completion.packet = make_http_event(op->id, **scoped);
        } else {
            const auto& error = (*scoped).error();
            guard.completion.packet =
                make_error_event(op->id, YADDNSC_STATUS_NETWORK_ERROR, http::failure_reason("API server", error),
                                 error.retry_after_seconds);
        }
    } catch (const coro::Cancelled&) {
        guard.completion.packet = make_error_event(op->id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0);
    } catch (...) {
        guard.completion.defect = std::current_exception();
    }
    guard.completion.op_id = op->id;
}

void cancel_all(const PumpState& state) {
    for (auto& [id, op] : state->ops) {
        if (op->ready) {
            continue;
        }
        if (op->scope != nullptr) {
            op->scope->cancel();
        } else {
            op->canceled = true;
        }
    }
}

void process_apply(PumpState& state, Command& command, coro::Nursery& group) {
    auto& wait = command.wait;
    try {
        if (command.terminal) {
            state->stopping = true;
            cancel_all(state);
            for (const auto& start : command.commands.starts) {
                wait->result.discarded.push_back(start.op_id);
            }
            command.commands.clear();
            wait->result.idle = state->ops.empty() && state->completions.empty();
            wait->done.notify();
            return;
        }

        for (const std::uint64_t op_id : command.commands.cancels) {
            const auto it = state->ops.find(op_id);
            if (it == state->ops.end() || it->second->ready) {
                continue;
            }
            if (it->second->scope != nullptr) {
                it->second->scope->cancel();
            } else {
                it->second->canceled = true;
            }
        }

        for (auto& start : command.commands.starts) {
            if (std::find(command.commands.cancels.begin(), command.commands.cancels.end(), start.op_id) !=
                command.commands.cancels.end()) {
                wait->result.discarded.push_back(start.op_id);
                state->completions.push_back(AsyncHttpCompletion{
                    start.op_id,
                    make_error_event(start.op_id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0),
                    {}});
                state->completion_changed.notify();
                continue;
            }

            auto op = std::make_shared<AsyncOp>();
            op->id = start.op_id;
            op->url = std::move(start.url);
            op->request = std::move(start.request);
            state->ops.emplace(op->id, op);
            ++state->active_tasks;
            try {
                group.spawn_discard(run_http(state, op));
            } catch (...) {
                --state->active_tasks;
                state->ops.erase(op->id);
                throw;
            }
        }
        command.commands.clear();
        wait->result.idle = state->ops.empty() && state->completions.empty();
        wait->done.notify();
    } catch (...) {
        wait->error = std::current_exception();
        wait->done.notify();
        throw;
    }
}

coro::Task<void> pump_main(PumpState state) {
    try {
        co_await coro::with_nursery(
            [&state](coro::Nursery& group) -> coro::Task<void> {
                for (;;) {
                    if (state->cancel_requested) {
                        cancel_all(state);
                    }
                    while (!state->commands.empty()) {
                        auto command = std::move(state->commands.front());
                        state->commands.pop_front();
                        switch (command.kind) {
                            case Command::Kind::APPLY:
                                process_apply(state, command, group);
                                break;
                            case Command::Kind::FINISH:
                                state->stopping = true;
                                cancel_all(state);
                                break;
                        }
                    }
                    if (state->finish_requested && state->active_tasks == 0) {
                        co_return;
                    }
                    co_await state->changed.wait();
                }
            },
            coro::NurseryPolicy::SUPERVISOR);
    } catch (...) {
        record_error(state, std::current_exception());
    }

    state->done = true;
    for (auto& command : state->commands) {
        if (command.wait) {
            command.wait->error =
                state->error
                    ? state->error
                    : std::make_exception_ptr(std::logic_error("async HTTP pump stopped before applying commands"));
            command.wait->done.notify();
        }
    }
    state->commands.clear();
    state->changed.notify();
    state->completion_changed.notify();
    state->finished.notify();
}

}  // namespace

AsyncEventPacket::AsyncEventPacket() {
    event.struct_size = static_cast<std::uint32_t>(sizeof(event));
    event.abi_major = YADDNSC_DRIVER_ABI_MAJOR;
    event.abi_minor = YADDNSC_DRIVER_ABI_MINOR;
}

AsyncHttpPump::AsyncHttpPump(http::Options options, const std::chrono::milliseconds budget)
    : state_(std::make_shared<State>(std::move(options), budget)) {}

coro::Task<void> AsyncHttpPump::run() {
    co_await pump_main(state_);
}

coro::Task<AsyncHttpPump::ApplyResult> AsyncHttpPump::apply(AsyncStepCommands commands, const bool terminal) {
    if (state_->error) {
        std::rethrow_exception(state_->error);
    }
    if (state_->done) {
        throw std::logic_error("async HTTP pump has stopped");
    }
    auto wait = std::make_shared<ApplyWait>();
    state_->commands.push_back(Command{Command::Kind::APPLY, std::move(commands), terminal, wait});
    state_->changed.notify();
    co_await wait->done.wait();
    if (wait->error) {
        std::rethrow_exception(wait->error);
    }
    co_return std::move(wait->result);
}

void AsyncHttpPump::cancel() noexcept {
    state_->cancel_requested = true;
    state_->changed.notify();
    state_->completion_changed.notify();
}

coro::Task<void> AsyncHttpPump::finish() {
    if (!state_->finish_requested) {
        state_->finish_requested = true;
        state_->stopping = true;
        try {
            state_->commands.push_back(Command{Command::Kind::FINISH});
        } catch (...) {
            record_error(state_, std::current_exception());
            state_->finished.notify();
        }
        state_->changed.notify();
    }
    if (!state_->done) {
        co_await state_->finished.wait();
    }
    if (state_->error) {
        std::rethrow_exception(state_->error);
    }
}

coro::Task<std::optional<AsyncHttpCompletion>> AsyncHttpPump::next_completion() {
    while (state_->completions.empty() && !state_->error && !state_->cancel_requested && !state_->done) {
        co_await state_->completion_changed.wait();
    }
    if (state_->error) {
        std::rethrow_exception(state_->error);
    }
    if (!state_->completions.empty()) {
        auto completion = std::move(state_->completions.front());
        state_->completions.pop_front();
        co_return completion;
    }
    co_return std::nullopt;
}

std::shared_ptr<AsyncEventPacket> make_cancel_event() {
    auto packet = std::make_shared<AsyncEventPacket>();
    packet->error_message = "update cancelled";
    packet->error.struct_size = static_cast<std::uint32_t>(sizeof(packet->error));
    packet->error.status = YADDNSC_STATUS_CANCELLED;
    packet->error.message = {packet->error_message.data(), packet->error_message.size()};
    packet->event.kind = YADDNSC_EVENT_UPDATE_CANCELLED;
    packet->event.op_id = 0;
    packet->event.status = YADDNSC_STATUS_CANCELLED;
    packet->event.response = nullptr;
    packet->event.error = &packet->error;
    return packet;
}

}  // namespace plugin
