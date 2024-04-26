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
    static constexpr int DOMAIN_NAME_MAX_LEN = 253;

    template<typename Enumeration>
    std::underlying_type_t<Enumeration> as_integer(Enumeration const value) {
        return static_cast<std::underlying_type_t<Enumeration>>(value);
    }

    template<typename T> requires (std::is_trivial_v<T> && !std::is_pointer_v<T>)
    size_t sizeof_obj(const T &obj) {
        return sizeof(obj);
    }

    template<class R, class E> requires std::is_base_of_v<YaddnscException, E>
    R retry_on_exception(const std::function<R()> &func, const unsigned retry,
                         const std::optional<std::function<bool(const E &)> > &e_filter = std::nullopt,
                         unsigned long backoff = 500) {
        unsigned counter = 0;
        while (true) {
            try {
                return func();
            } catch (const E &e) {
                // apply filter if available
                if (e_filter.has_value() && !e_filter.value()(e)) {
                    throw;
                }
                if (++counter > retry) {
                    throw;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff * counter));
                SPDLOG_DEBUG("{} exception caught, retrying...(counter {})", e.get_name(), counter);
            }
        }
    }

    inline bool is_valid_domain(std::string_view domain) {
        const static std::regex domain_regex(
            "^(((?!-))(xn--|_)?[a-z0-9-]{0,61}[a-z0-9]{1,1}\\.)*(xn--)?([a-z0-9][a-z0-9\\-]{0,60}|[a-z0-9-]{1,30}\\.[a-z]{2,})\\.?$");

        if (domain.length() > DOMAIN_NAME_MAX_LEN) {
            return false;
        }

        if (domain.find(".") != std::string_view::npos) {
            std::cmatch match;
            std::regex_match(domain.data(), match, domain_regex);

            return !match.empty();
        }

        return false;
    }
}

#endif //YADDNSC_UTIL_H
