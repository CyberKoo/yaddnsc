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
    /// Concept for types that can be hashed via std::hash.
    template<typename T>
    concept Hashable = requires(T t) {
        { std::hash<T>{}(t) } -> std::convertible_to<std::size_t>;
    };

    /// Remove duplicate elements from a vector in place.
    ///
    /// Uses an std::unordered_set for O(n) deduplication.  The relative order
    /// of remaining elements is preserved (first occurrence wins).
    ///
    /// @tparam T  Hashable element type.
    /// @param vec  Vector to deduplicate in place.
    /// @return     The number of elements removed.
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
