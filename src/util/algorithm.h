//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_UTIL_ALGORITHM_H
#define YADDNSC_UTIL_ALGORITHM_H

#include <vector>
#include <type_traits>
#include <unordered_set>

namespace Utils {
    template<typename T>
    void dedupe(std::vector<T> &vec) {
        std::unordered_set<T> seen;
        std::erase_if(vec, [&seen](const T &value) {
            return !seen.insert(value).second;
        });
    }
}

#endif // YADDNSC_UTIL_ALGORITHM_H
