#ifndef YADDNSC_SUPPORT_UTIL_ALGORITHM_HPP
#define YADDNSC_SUPPORT_UTIL_ALGORITHM_HPP

#include <cstddef>
#include <unordered_set>
#include <vector>

#include <concepts>

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
std::size_t dedupe(std::vector<T>& vec) {
    std::unordered_set<T> seen;
    seen.reserve(vec.size());

    const auto before = vec.size();
    std::erase_if(vec, [&seen](const T& value) { return !seen.insert(value).second; });

    return before - vec.size();
}
}  // namespace Utils

#endif  // YADDNSC_SUPPORT_UTIL_ALGORITHM_HPP
