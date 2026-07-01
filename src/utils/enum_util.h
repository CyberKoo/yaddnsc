//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_UTIL_ENUM_UTIL_H
#define YADDNSC_UTIL_ENUM_UTIL_H

#include <type_traits>

namespace Utils {
    template<typename Enumeration>
    std::underlying_type_t<Enumeration> as_integer(Enumeration const value) {
        return static_cast<std::underlying_type_t<Enumeration>>(value);
    }
}

#endif // YADDNSC_UTIL_ENUM_UTIL_H
