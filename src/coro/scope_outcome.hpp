//
// Coroutine runtime — the outcome of a cancellation combinator.
//
// Split from scope.hpp for the same reason as TaskGroup: the combinators' body
// builds a ScopeOutcome and therefore needs it complete, so it lives in a public
// header that detail/scope_runner.hpp can include without a cycle.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_SCOPE_OUTCOME_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_SCOPE_OUTCOME_HPP

#include <optional>

#include "coro/fwd.h"

namespace coro {

/// Body result plus the scope state that produced it.
///
/// Ownership: owns the body's value. A combinator always waits for its body, so
/// a completed body has an engaged `value`; absorbed own cancellation has no
/// value. Defects and ancestor cancellation are rethrown.
/// Failure: reading `*outcome` when `has_value()` is false is a precondition
/// violation.
template<typename T>
struct ScopeOutcome {
    std::optional<T> value;
    /// The combinator's own deadline fired.
    bool timed_out = false;
    /// The combinator absorbed its own cancellation; ancestors propagate.
    bool cancelled = false;

    /// True when the body produced a value.
    [[nodiscard]] bool has_value() const noexcept { return value.has_value(); }

    /// Same as has_value(): the body completed and yielded a value.
    [[nodiscard]] explicit operator bool() const noexcept { return value.has_value(); }

    /// The body's value. Precondition: has_value().
    [[nodiscard]] T& operator*() noexcept { return *value; }

    /// The body's value. Precondition: has_value().
    [[nodiscard]] const T& operator*() const noexcept { return *value; }

    /// The body's value. Precondition: has_value().
    [[nodiscard]] T* operator->() noexcept { return &*value; }

    /// The body's value. Precondition: has_value().
    [[nodiscard]] const T* operator->() const noexcept { return &*value; }
};

/// void specialization: completion is the only observable result.
template<>
struct ScopeOutcome<void> {
    bool completed = false;
    bool timed_out = false;
    bool cancelled = false;

    [[nodiscard]] explicit operator bool() const noexcept { return completed; }
};

}  // namespace coro

#endif  // YADDNSC_INFRASTRUCTURE_CORO_SCOPE_OUTCOME_HPP