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
        template<typename Pred, typename E>
        concept invocable_pred_for = std::predicate<Pred, const E &>;
    }

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
