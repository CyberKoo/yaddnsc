// Coroutine runtime — runner-owned nursery resources and its single close path.
#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NURSERY_RUNNER_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NURSERY_RUNNER_HPP

#include <exception>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "coro/detail/access.h"
#include "coro/detail/context_awaitables.h"
#include "coro/detail/nursery_state.hpp"
#include "coro/group_error.h"
#include "coro/task.hpp"
#include "coro/nursery.hpp"

namespace coro::detail {

/// Inputs are owned by value at their final addresses before invoking Fn&.
/// Optional storage permits explicit Fn -> Resources -> state release on every
/// exit, including an allocation failure during post-drain error aggregation.
template<typename Resources, typename Fn>
Task<void> run_nursery(std::optional<Resources> resources, std::optional<Fn> fn, NurseryPolicy policy) {
    auto& execution = co_await CurrentRecord{};
    std::optional<NurseryState> storage;

    struct Cleanup {
        std::optional<Resources>& resources;
        std::optional<Fn>& fn;
        std::optional<NurseryState>& state;

        ~Cleanup() noexcept {
            if (state) {
                state->clear_results();
            }
            fn.reset();
            resources.reset();
            state.reset();
        }
    } cleanup{resources, fn, storage};

    auto& state = storage.emplace(execution.loop(), execution.scope(), policy);
    state.runner = &execution;
    Nursery nursery{&state};

    std::exception_ptr body_error;
    bool body_cancelled = false;
    try {
        Task<void> body = std::invoke(*fn, nursery, *resources);
        co_await TaskAccess::await_in_scope(body, *execution.loop(), state.scope);
    } catch (const Cancelled&) {
        body_error = std::current_exception();
        body_cancelled = true;
    } catch (...) {
        body_error = std::current_exception();
    }

    state.close();
    if (body_error) {
        ScopeAccess::cancel(state.scope, CancelCause::REQUESTED);
    }
    co_await AllDoneAwaitable{&state};

    if (!state.supervisor) {
        if (state.first_error) {
            std::rethrow_exception(state.first_error);
        }
    } else {
        auto unobserved = state.unobserved_errors();
        if (!unobserved.empty()) {
            if (body_error && !body_cancelled) {
                unobserved.push_back(body_error);
            }
            throw GroupError{std::move(unobserved)};
        }
    }
    try {
        if (body_error) {
            std::rethrow_exception(body_error);
        }
        state.scope.throw_if_cancelled();
    } catch (const Cancelled& cancellation) {
        if (!ScopeAccess::absorbs(state.scope, cancellation)) {
            state.scope.throw_if_cancelled();
            throw;
        }
    }
}

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_NURSERY_RUNNER_HPP
