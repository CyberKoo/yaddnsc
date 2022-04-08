//
// Created by Kotarou on 2022/4/6.
//

#ifndef YADDNSC_UTIL_H
#define YADDNSC_UTIL_H

#include<type_traits>

namespace Util {
    template<typename Enumeration>
    typename std::underlying_type<Enumeration>::type as_integer(Enumeration const value) {
        return static_cast<typename std::underlying_type<Enumeration>::type>(value);
    }
}

#endif //YADDNSC_UTIL_H
