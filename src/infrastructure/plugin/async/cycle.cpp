#include "cycle.h"

#include <atomic>
#include <exception>
#include <expected>
#include <string_view>
#include <utility>

#include "coro/cancel_scope.h"
#include "coro/nursery.hpp"
#include "coro/scope.hpp"
#include "coro/with_nursery.hpp"
#include "infrastructure/plugin/async/cleanup.h"
#include "infrastructure/plugin/async/cycle_state.h"
#include "infrastructure/plugin/async/http.h"
#include "infrastructure/plugin/async/step.h"
#include "infrastructure/plugin/detail/abi_result.h"
#include "infrastructure/plugin/detail/cycle.h"
#include "infrastructure/plugin/plugin_loader.h"

namespace plugin::detail {
namespace {

bool finish_update_step(AsyncResources& res, const AsyncStepOutcome& outcome, std::string_view entry) {
    return finish_async_step(*res.state, res.module->descriptor().name, outcome, entry, res.command.fqdn);
}

/// Loop-side protocol owns entry ordering. HTTP admission/publication belongs
/// to the event pump; each entry is a shielded joined step, not an HTTP wait.
coro::Task<void> async_cycle_body(AsyncResourcePtr resource) {
    AsyncResources& res = *resource;
    try {
        if (res.state->cancelled.load(std::memory_order_acquire)) {
            res.state->plugin_cancelled = true;
            co_return;
        }
        co_await coro::with_nursery(
            resource,
            [](coro::Nursery& http_group, AsyncResourcePtr& shared) -> coro::Task<void> {
                AsyncResources& r = *shared;
                struct StopGuard {
                    AsyncResources& r;
                    ~StopGuard() {
                        // Release the watcher and stop HTTP on every body exit.
                        r.stopping = true;
                        cancel_all_async_ops(r);
                        r.state->owner_changed.notify();
                        r.state->cancel_changed.notify();
                    }
                } stop_guard{r};
                http_group.spawn_discard(watch_async_cancellation(shared));
                auto create = co_await run_async_step([&r] { return create_async_step(r); });
                if (!finish_update_step(r, create, "async create")) {
                    co_return;
                }
                if (r.state->cancelled.load(std::memory_order_acquire)) {
                    r.state->plugin_cancelled = true;
                    co_return;
                }
                auto begin = co_await run_async_step([&r] { return begin_async_step(r); });
                if (begin.status != YADDNSC_STATUS_PENDING) {
                    apply_async_commands(shared, http_group, true);
                    finish_update_step(r, begin, "async begin");
                    co_return;
                }
                apply_async_commands(shared, http_group, false);
                if (r.ops.empty() && r.completions.empty()) {
                    r.state->result =
                        std::unexpected(invalid_plugin_result(r.module->descriptor().name, "async begin"));
                    co_return;
                }
                while (true) {
                    if (r.state->error) {
                        break;
                    }
                    if (r.state->cancelled.load(std::memory_order_acquire)) {
                        r.stopping = true;
                        cancel_all_async_ops(r);
                        auto cancel_event = make_cancel_event();
                        auto outcome =
                            co_await run_async_step([&r, cancel_event] { return resume_async_step(r, cancel_event); });
                        if (outcome.status == YADDNSC_STATUS_PENDING) {
                            r.state->result =
                                std::unexpected(invalid_plugin_result(r.module->descriptor().name, "async cancel"));
                        } else {
                            finish_update_step(r, outcome, "async cancel");
                        }
                        break;
                    }
                    if (!r.completions.empty()) {
                        auto event = std::move(r.completions.front());
                        r.completions.pop_front();
                        if (event->event.kind == YADDNSC_EVENT_HTTP_FINISHED) {
                            r.ops.erase(event->event.op_id);
                            r.services->mark_delivered(event->event.op_id);
                        }
                        auto outcome = co_await run_async_step([&r, event] { return resume_async_step(r, event); });
                        if (outcome.status == YADDNSC_STATUS_PENDING) {
                            if (r.state->cancelled.load(std::memory_order_acquire)) {
                                r.stopping = true;
                                cancel_all_async_ops(r);
                                auto cancel_event = make_cancel_event();
                                auto cancel_outcome = co_await run_async_step(
                                    [&r, cancel_event] { return resume_async_step(r, cancel_event); });
                                if (cancel_outcome.status == YADDNSC_STATUS_PENDING) {
                                    r.state->result = std::unexpected(
                                        invalid_plugin_result(r.module->descriptor().name, "async cancel"));
                                } else {
                                    finish_update_step(r, cancel_outcome, "async cancel");
                                }
                                break;
                            }
                            apply_async_commands(shared, http_group, false);
                            if (r.ops.empty() && r.completions.empty()) {
                                r.state->result =
                                    std::unexpected(invalid_plugin_result(r.module->descriptor().name, "async resume"));
                                break;
                            }
                            continue;
                        }
                        apply_async_commands(shared, http_group, true);
                        finish_update_step(r, outcome, "async resume");
                        break;
                    }
                    co_await r.state->owner_changed.wait();
                }
                r.stopping = true;
                cancel_all_async_ops(r);
                while (r.active_tasks != 0) {
                    co_await r.state->owner_changed.wait();
                }
                co_await run_async_step([&r] { return destroy_async_step(r); });
            },
            coro::NurseryPolicy::SUPERVISOR);
    } catch (...) {
        if (!res.state->error) {
            res.state->error = std::current_exception();
        }
    }
    if (res.handle != nullptr) {
        try {
            co_await run_async_step([&res] { return destroy_async_step(res); });
        } catch (...) {
            // Never release module/services while a handle remains unaccounted.
            fatal_async_cleanup_failure(res.module->descriptor().name, "async destroy step did not complete");
        }
    }
    res.state->result_ready.notify();
    while (res.state->subscriber_active) {
        co_await res.state->owner_changed.wait();
    }
}

}  // namespace
}  // namespace plugin::detail

namespace plugin {

coro::Task<void> make_async_update_owner(std::shared_ptr<CallState> state, std::shared_ptr<const PluginModule> module,
                                         domain::DriverUpdateCommand command, const app::LoggerPort& logger,
                                         http::Options options, std::chrono::milliseconds budget) {
    auto resource = std::make_shared<detail::AsyncResources>(state, std::move(module), std::move(command), logger,
                                                             std::move(options), budget);
    auto shielded = coro::non_cancellable(
        [body = detail::async_cycle_body(std::move(resource))](coro::CancelScope&) mutable { return std::move(body); });
    return own_cycle(std::move(state), std::move(shielded));
}

}  // namespace plugin
