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

namespace Utils::Retry {
    namespace detail {
        /// Concept for a predicate that can filter errors.
        template<typename Pred, typename E>
        concept invocable_pred_for = std::predicate<Pred, const E &>;
    }

    /// Retry a callable that returns std::expected<R, E> with backoff.
    ///
    /// Invokes the callable in a loop.  If the callable returns
    /// std::expected with a value, returns it immediately.  If it returns
    /// an error, the optional predicate is consulted: return true to
    /// retry, false to propagate the error immediately.  Retries up to
    /// `retry` times with linear backoff (backoff * retry_count).
    ///
    /// Callables that need to handle transport-level exceptions should
    /// catch them internally and return std::unexpected<E>.
    ///
    /// @tparam R       Success return type of the callable.
    /// @tparam E       Error type in std::expected<R, E>.
    /// @tparam Fn      Callable type returning std::expected<R, E>.
    /// @tparam Pred    Predicate type `bool(const E&)`, or std::nullopt_t to retry all.
    ///
    /// @param func           The callable to invoke and retry.
    /// @param retry          Maximum number of retries before giving up.
    /// @param e_filter       Optional predicate: return true to retry, false to propagate.
    /// @param backoff        Base backoff interval in milliseconds (multiplied by retry count).
    /// @param actual_retries Optional output: the number of retries actually performed.
    ///
    /// @return  std::expected<R, E> — the result on success, or the last error.
    template<class R, class E, std::invocable<> Fn, typename Pred = std::nullopt_t>
        requires (std::same_as<Pred, std::nullopt_t> || detail::invocable_pred_for<Pred, E>)
    std::expected<R, E> retry_on_error(Fn &&func, unsigned retry, Pred &&e_filter = std::nullopt,
                                           unsigned long backoff = 500, unsigned *actual_retries = nullptr) {
        static_assert(std::is_same_v<decltype(func()), std::expected<R, E>>,
                      "Fn must return std::expected<R, E>");

        unsigned counter = 0;
        while (true) {
            if (actual_retries) *actual_retries = counter;

            auto result = func();
            if (result) {
                return result;
            }

            // Apply filter if available — returns true to retry, false to propagate.
            if constexpr (!std::same_as<std::decay_t<Pred>, std::nullopt_t>) {
                if (!e_filter(result.error())) {
                    if (actual_retries) *actual_retries = counter;
                    return result;
                }
            }

            if (++counter > retry) {
                if (actual_retries) *actual_retries = counter - 1;
                return result;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(backoff * counter));
            SPDLOG_DEBUG("retrying... (counter {})", counter);
        }
    }
} // namespace Utils::Retry

#endif // YADDNSC_UTIL_RETRY_UTIL_H
