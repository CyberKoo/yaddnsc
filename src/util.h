//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_UTIL_H
#define YADDNSC_UTIL_H

#include <thread>
#include <functional>
#include <type_traits>

#include "exception/base_exception.h"

namespace Util {
    template<typename Enumeration>
    typename std::underlying_type<Enumeration>::type as_integer(Enumeration const value) {
        return static_cast<typename std::underlying_type<Enumeration>::type>(value);
    }

    template<class R, class E, class=std::enable_if_t<std::is_base_of_v<YaddnscException, E>>>
    R retry_on_exception(const std::function<R()> &func, const unsigned retry,
                         const std::optional<std::function<bool(const E &)>> &e_filter = std::nullopt,
                         unsigned long backoff = 500) {
        unsigned counter = 0;
        while (true) {
            try {
                return func();
            } catch (const E &e) {
                // apply filter if available
                if (e_filter.has_value() && !e_filter.value()(e)) {
                    throw e;
                } else {
                    if (++counter > retry) {
                        throw e;
                    } else {
                        std::this_thread::sleep_for(std::chrono::milliseconds(backoff * counter));
                        SPDLOG_DEBUG("{} exception caught, retrying...(counter {})", e.get_name(), counter);
                    }
                }
            }
        }
    }
}

#endif //YADDNSC_UTIL_H
