//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_UTIL_ALGORITHM_H
#define YADDNSC_UTIL_ALGORITHM_H

#include <vector>
#include <cstddef>
#include <concepts>
#include <unordered_set>

namespace Utils {
    template<typename T>
    concept Hashable = requires(T t) {
        { std::hash<T>{}(t) } -> std::convertible_to<std::size_t>;
    };

    template<Hashable T>
    std::size_t dedupe(std::vector<T> &vec) {
        std::unordered_set<T> seen;
        seen.reserve(vec.size());

        const auto before = vec.size();
        std::erase_if(vec, [&seen](const T &value) {
            return !seen.insert(value).second;
        });

        return before - vec.size();
    }
}

#endif // YADDNSC_UTIL_ALGORITHM_H
