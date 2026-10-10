//
// Coroutine runtime — void-safe result storage.
//
// std::optional<void> does not exist, so the primitives that may return either a
// value or nothing (offload, the scope combinators) share this small box instead
// of duplicating a partial specialization each time.
//

#ifndef YADDNSC_INFRASTRUCTURE_CORO_DETAIL_RESULT_BOX_HPP
#define YADDNSC_INFRASTRUCTURE_CORO_DETAIL_RESULT_BOX_HPP

#include <optional>
#include <utility>

namespace coro::detail {

/// Storage for a callable's result.
///
/// Not thread-safe; the owner synchronizes hand-offs (the offload cell
/// publishes it through an atomic release/acquire pair).
template<typename R>
struct ResultBox {
    std::optional<R> value;

    /// Stores the callable's result. May throw if the callable or R's move
    /// constructor throws; the box is left empty in that case.
    template<typename Fn>
    void invoke(Fn& fn) {
        value.emplace(fn());
    }

    /// Moves the result out. Precondition: invoke() succeeded.
    [[nodiscard]] R take() { return std::move(*value); }
};

/// void specialization: nothing to store, nothing to move out.
template<>
struct ResultBox<void> {
    /// Calls the callable and discards nothing (the result is void).
    template<typename Fn>
    void invoke(Fn& fn) {
        fn();
    }

    /// No-op, so callers can stay type-generic.
    void take() const noexcept {}
};

}  // namespace coro::detail

#endif  // YADDNSC_INFRASTRUCTURE_CORO_DETAIL_RESULT_BOX_HPP
