//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_UTIL_RETRY_UTIL_H
#define YADDNSC_UTIL_RETRY_UTIL_H

#include <thread>
#include <type_traits>
#include <chrono>

#include <spdlog/spdlog.h>

#include "exception/base_exception.h"

namespace Util {
    namespace detail {
        template<typename Pred, typename E>
        concept invocable_pred_for = std::predicate<Pred, const E &>;
    }

    template<class R, class E, std::invocable<> Fn, typename Pred = std::nullopt_t>
        requires std::is_base_of_v<YaddnscException, E> &&
                 (std::same_as<Pred, std::nullopt_t> || detail::invocable_pred_for<Pred, E>)
    R retry_on_exception(Fn &&func, const unsigned retry,
                         Pred &&e_filter = std::nullopt,
                         unsigned long backoff = 500) {
        unsigned counter = 0;
        while (true) {
            try {
                return func();
            } catch (const E &e) {
                // apply filter if available
                if constexpr (!std::same_as<std::decay_t<Pred>, std::nullopt_t>) {
                    if (!std::forward<Pred>(e_filter)(e)) {
                        throw;
                    }
                }
                if (++counter > retry) {
                    throw;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff * counter));
                SPDLOG_DEBUG("{} exception caught, retrying...(counter {})", e.get_name(), counter);
            }
        }
    }
}

#endif // YADDNSC_UTIL_RETRY_UTIL_H
