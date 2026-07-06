//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_UTIL_RETRY_UTIL_H
#define YADDNSC_UTIL_RETRY_UTIL_H

#include <chrono>
#include <thread>
#include <expected>
#include <type_traits>

#include <spdlog/spdlog.h>

#include "exception/base.h"

namespace Utils::Retry {
    namespace detail {
        /// Concept for a predicate that can filter exceptions.
        template<typename Pred, typename E>
        concept invocable_pred_for = std::predicate<Pred, const E &>;
    }

    /// Retry a callable on a specific exception type with backoff.
    ///
    /// Catches exceptions of type `E` (which must inherit from YaddnscException)
    /// and retries the function up to `retry` times with linear backoff.
    /// An optional predicate can be provided to filter which exceptions trigger
    /// a retry vs. being propagated immediately.
    ///
    /// @tparam R       Success return type of the callable.
    /// @tparam E       Exception type to catch (must inherit from YaddnscException).
    /// @tparam Fn      Callable type returning std::expected<R, E>.
    /// @tparam Pred    Predicate type `bool(const E&)`, or std::nullopt_t to retry all.
    ///
    /// @param func           The callable to invoke and retry.
    /// @param retry          Maximum number of retries before giving up.
    /// @param e_filter       Optional predicate: return true to retry, false to propagate.
    /// @param backoff        Base backoff interval in milliseconds (multiplied by retry count).
    /// @param actual_retries Optional output: the number of retries actually performed.
    ///
    /// @return  std::expected<R, E> — the result on success, or the last exception.
    template<class R, class E, std::invocable<> Fn, typename Pred = std::nullopt_t>
        requires std::is_base_of_v<YaddnscException, E> &&
                 (std::same_as<Pred, std::nullopt_t> || detail::invocable_pred_for<Pred, E>)
    std::expected<R, E> retry_on_exception(Fn &&func, unsigned retry, Pred &&e_filter = std::nullopt,
                                           unsigned long backoff = 500, unsigned *actual_retries = nullptr) {
        unsigned counter = 0;
        while (true) {
            try {
                if (actual_retries) *actual_retries = counter;
                return func();
            } catch (const E &e) {
                // apply filter if available
                // NOTE: e_filter is called as an lvalue — the retry loop may invoke
                // it multiple times, so we must not forward/move from it.
                if constexpr (!std::same_as<std::decay_t<Pred>, std::nullopt_t>) {
                    if (!e_filter(e)) {
                        if (actual_retries) *actual_retries = counter;
                        return std::unexpected(e);
                    }
                }
                if (++counter > retry) {
                    if (actual_retries) *actual_retries = counter - 1;
                    return std::unexpected(e);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff * counter));
                SPDLOG_DEBUG("{} exception caught, retrying...(counter {})", e.get_name(), counter);
            }
        }
    }
} // namespace Utils::Retry

#endif // YADDNSC_UTIL_RETRY_UTIL_H
