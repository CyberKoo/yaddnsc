// Coroutine runtime — structured resource ownership and child supervision.
#ifndef YADDNSC_CORO_WITH_NURSERY_HPP
#define YADDNSC_CORO_WITH_NURSERY_HPP

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>

#include <concepts>

#include "coro/detail/nursery_runner.hpp"
#include "coro/group_error.h"  // IWYU pragma: export
#include "coro/task.hpp"
#include "coro/nursery.hpp"

namespace coro {

/// Own resources and a body factory until every child has fully retired.
///
/// Calls Fn& with Nursery& and Resources& at stable addresses. Inputs are taken
/// by value, including move-only inputs; reference/pointer members still borrow
/// their backing owners, which must cover the complete nursery await. Body
/// locals and parameters are not retained for children after body return.
/// Normal return closes admission and drains; abnormal return cancels then
/// drains. Cleanup invalidates results, releases Fn, Resources, then state.
/// FAIL_FAST propagates the first child defect; SUPERVISOR aggregates unobserved
/// defects as GroupError. Body defects, ancestor cancellation and allocation
/// failures propagate only after necessary cleanup. Thread safety: loop only.
template<typename Resources, typename Fn>
    requires requires(Fn& fn, Nursery& nursery, Resources& resources) {
        { std::invoke(fn, nursery, resources) } -> std::same_as<Task<void>>;
    }
[[nodiscard]] Task<void> with_nursery(Resources resources, Fn fn, NurseryPolicy policy = NurseryPolicy::FAIL_FAST) {
    return detail::run_nursery(std::optional<Resources>{std::move(resources)}, std::optional<Fn>{std::move(fn)},
                               policy);
}

/// Resource-free form: Fn& receives Nursery& and must return Task<void>.
/// The same lifetime, cancellation, error and thread-safety rules apply.
template<typename Fn>
    requires requires(Fn& fn, Nursery& nursery) {
        { std::invoke(fn, nursery) } -> std::same_as<Task<void>>;
    }
[[nodiscard]] Task<void> with_nursery(Fn fn, NurseryPolicy policy = NurseryPolicy::FAIL_FAST) {
    return with_nursery(
        nullptr, [fn = std::move(fn)](Nursery& nursery, std::nullptr_t&) mutable { return std::invoke(fn, nursery); },
        policy);
}

}  // namespace coro

#endif  // YADDNSC_CORO_WITH_NURSERY_HPP
