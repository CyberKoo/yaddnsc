#include "http.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <expected>
#include <string>
#include <utility>

#include <yaddnsc/sdk/driver_abi.h>

#include "coro/cancel_scope.h"
#include "coro/cancelled.h"
#include "coro/now.hpp"
#include "coro/nursery.hpp"
#include "coro/scope.hpp"
#include "infrastructure/http/client.h"
#include "infrastructure/http/error.h"
#include "infrastructure/plugin/async/cycle_state.h"

namespace plugin::detail {
namespace {

std::shared_ptr<AsyncEventPacket> make_http_event(uint64_t op_id, const http::Response& response) {
    auto packet = std::make_shared<AsyncEventPacket>();
    packet->response.struct_size = static_cast<uint32_t>(sizeof(packet->response));
    packet->response.status_code = static_cast<uint32_t>(response.status_);
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
    packet->response.body = {reinterpret_cast<const uint8_t*>(packet->body.data()), packet->body.size()};
    packet->event.kind = YADDNSC_EVENT_HTTP_FINISHED;
    packet->event.op_id = op_id;
    packet->event.status = YADDNSC_STATUS_OK;
    packet->event.response = &packet->response;
    packet->event.error = nullptr;
    return packet;
}

std::shared_ptr<AsyncEventPacket> make_error_event(uint64_t op_id, yaddnsc_status status, std::string message,
                                                   uint32_t retry_after_seconds) {
    auto packet = std::make_shared<AsyncEventPacket>();
    packet->error_message = std::move(message);
    packet->error.struct_size = static_cast<uint32_t>(sizeof(packet->error));
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

coro::Task<void> run_async_http(AsyncResourcePtr resource, std::shared_ptr<AsyncOp> op) {
    AsyncResources& res = *resource;

    struct ActiveTaskGuard {
        AsyncResources& res;

        ~ActiveTaskGuard() {
            if (res.active_tasks != 0) {
                --res.active_tasks;
            }
            res.state->owner_changed.notify();
        }
    } guard{res};

    std::shared_ptr<AsyncEventPacket> packet;
    try {
        if (res.stopping || op->canceled || res.state->cancelled.load(std::memory_order_acquire)) {
            packet = make_error_event(op->id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0);
        } else {
            auto scoped = co_await coro::with_cancel_scope(
                [&res, op](coro::CancelScope& scope) -> coro::Task<std::expected<http::Response, http::Error>> {
                    op->scope = &scope;
                    struct ScopeBorrow {
                        AsyncOp& op;
                        ~ScopeBorrow() noexcept { op.scope = nullptr; }
                    } borrow{*op};
                    if (res.stopping || op->canceled || res.state->cancelled.load(std::memory_order_acquire)) {
                        scope.cancel();
                    }
                    scope.throw_if_cancelled();
                    const auto deadline = co_await coro::current_time() + res.budget;
                    auto outcome = co_await coro::with_deadline(
                        deadline, [&res, op]() -> coro::Task<std::expected<http::Response, http::Error>> {
                            http::Client client{res.http};
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
                packet = make_error_event(op->id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0);
            } else if (scoped.timed_out) {
                packet =
                    make_error_event(op->id, YADDNSC_STATUS_NETWORK_ERROR, "the API server did not respond in time", 0);
            } else if (!scoped.has_value()) {
                packet =
                    make_error_event(op->id, YADDNSC_STATUS_INTERNAL_ERROR, "async HTTP task produced no result", 0);
            } else if (*scoped) {
                packet = make_http_event(op->id, **scoped);
            } else {
                const auto& error = (*scoped).error();
                packet = make_error_event(op->id, YADDNSC_STATUS_NETWORK_ERROR,
                                          http::failure_reason("API server", error), error.retry_after_seconds);
            }
        }
    } catch (const coro::Cancelled&) {
        packet = make_error_event(op->id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0);
    } catch (...) {
        if (!res.state->error) {
            res.state->error = std::current_exception();
        }
        res.state->owner_changed.notify();
        co_return;
    }
    try {
        op->ready = true;
        res.completions.push_back(std::move(packet));
    } catch (...) {
        if (!res.state->error) {
            res.state->error = std::current_exception();
        }
    }
    res.state->owner_changed.notify();
    co_return;
}

}  // namespace

std::shared_ptr<AsyncEventPacket> make_cancel_event() {
    auto packet = std::make_shared<AsyncEventPacket>();
    packet->error_message = "update cancelled";
    packet->error.struct_size = static_cast<uint32_t>(sizeof(packet->error));
    packet->error.status = YADDNSC_STATUS_CANCELLED;
    packet->error.message = {packet->error_message.data(), packet->error_message.size()};
    packet->event.kind = YADDNSC_EVENT_UPDATE_CANCELLED;
    packet->event.op_id = 0;
    packet->event.status = YADDNSC_STATUS_CANCELLED;
    packet->event.response = nullptr;
    packet->event.error = &packet->error;
    return packet;
}

void cancel_all_async_ops(AsyncResources& res) {
    for (auto& [id, op] : res.ops) {
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

void apply_async_commands(AsyncResourcePtr resource, coro::Nursery& http_group, bool terminal) {
    AsyncResources& res = *resource;
    if (terminal) {
        res.stopping = true;
        cancel_all_async_ops(res);
        for (auto& start : res.commands.starts) {
            res.services->mark_delivered(start.op_id);
        }
        res.commands.clear();
        return;
    }
    for (const uint64_t op_id : res.commands.cancels) {
        const auto it = res.ops.find(op_id);
        if (it == res.ops.end() || it->second->ready) {
            continue;  // A late cancel cannot overwrite READY or delivered work.
        }
        if (it->second->scope != nullptr) {
            it->second->scope->cancel();
        } else {
            it->second->canceled = true;
        }
    }
    for (auto& start : res.commands.starts) {
        if (std::find(res.commands.cancels.begin(), res.commands.cancels.end(), start.op_id) !=
            res.commands.cancels.end()) {
            res.services->mark_delivered(start.op_id);
            res.completions.push_back(
                make_error_event(start.op_id, YADDNSC_STATUS_CANCELLED, "the request was cancelled", 0));
            continue;
        }
        auto op = std::make_shared<AsyncOp>();
        op->id = start.op_id;
        op->url = std::move(start.url);
        op->request = std::move(start.request);
        res.ops.emplace(op->id, op);
        ++res.active_tasks;
        try {
            http_group.spawn_discard(run_async_http(resource, op));
        } catch (...) {
            --res.active_tasks;
            throw;
        }
    }
    res.commands.clear();
}

coro::Task<void> watch_async_cancellation(AsyncResourcePtr resource) {
    AsyncResources& res = *resource;
    while (!res.stopping && !res.state->error) {
        if (res.state->cancelled.load(std::memory_order_acquire)) {
            res.stopping = true;
            cancel_all_async_ops(res);
            res.state->owner_changed.notify();
            co_return;
        }
        try {
            co_await res.state->cancel_changed.wait();
        } catch (const coro::Cancelled&) {
            co_return;
        }
    }
}

}  // namespace plugin::detail
