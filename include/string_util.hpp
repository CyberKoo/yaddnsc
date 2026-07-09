//
// Created by Kotarou on 2022/4/7.
//

#ifndef YADDNSC_STRING_UTIL_H
#define YADDNSC_STRING_UTIL_H

#include <vector>
#include <string>
#include <ranges>
#include <cctype>
#include <array>
#include <cstdint>
#include <concepts>
#include <algorithm>
#include <string_view>
#include <stdexcept>

namespace StringUtil {
    // ── Concepts ──────────────────────────────────────────────────────────────────

    /// Any contiguous char buffer that can be mutated (std::string, std::pmr::string, std::vector<char>, etc.).
    /// @tparam T  Type satisfying contiguous_range with char value_type.
    template<typename T>
    concept MutableCharBuffer = std::ranges::contiguous_range<T> && std::same_as<std::ranges::range_value_t<T>, char>;

    /// Any type that can be viewed as a string_view (const char*, std::string, std::string_view, etc.).
    /// @tparam T  Type constructible to std::string_view from const T&.
    template<typename T>
    concept StringViewable = std::is_constructible_v<std::string_view, const T &>;

    // ── Implementation details ────────────────────────────────────────────────────

    namespace detail {
        // Single definition to avoid repeated instantiation in each template
        inline constexpr std::array<char, 256> lower_table = []() noexcept {
            std::array<char, 256> table{};
            for (size_t i = 0; i < 256; ++i)
                table[i] = (i >= 'A' && i <= 'Z') ? static_cast<char>(i + 32) : static_cast<char>(i);
            return table;
        }();

        inline constexpr std::array<char, 256> upper_table = []() noexcept {
            std::array<char, 256> table{};
            for (size_t i = 0; i < 256; ++i)
                table[i] = (i >= 'a' && i <= 'z') ? static_cast<char>(i - 32) : static_cast<char>(i);
            return table;
        }();

        /// A range of pairs whose first and second elements are string-viewable.
        /// @tparam T  Input range with .first and .second members satisfying StringViewable.
        template<typename T>
        concept PairViewable =
                std::ranges::input_range<const T> &&
                requires(std::ranges::range_value_t<const T> v)
                {
                    { v.first } -> StringViewable;
                    { v.second } -> StringViewable;
                };
    } // namespace detail

    // ── In-place case conversion ──────────────────────────────────────────────────

    /// Convert all characters in a mutable buffer to lowercase in place.
    /// @tparam T  A MutableCharBuffer type (e.g. std::string&).
    template<MutableCharBuffer T>
    void to_lower(T &buf) noexcept {
        std::ranges::transform(buf, buf.begin(), [&](char c) noexcept {
            return detail::lower_table[static_cast<std::uint8_t>(c)];
        });
    }

    /// Convert all characters in a mutable buffer to uppercase in place.
    /// @tparam T  A MutableCharBuffer type (e.g. std::string&).
    template<MutableCharBuffer T>
    void to_upper(T &buf) noexcept {
        std::ranges::transform(buf, buf.begin(), [&](char c) noexcept {
            return detail::upper_table[static_cast<std::uint8_t>(c)];
        });
    }

    // ── Copying case conversion ───────────────────────────────────────────────────

    /// Return a lowercased copy of a string-like value.
    /// @tparam T  A StringViewable type.
    /// @return    A new std::string with all characters lowered.
    template<StringViewable T>
    std::string to_lower_copy(T &&str) {
        std::string new_str(std::forward<T>(str));
        to_lower(new_str);
        return new_str;
    }

    /// Return an uppercased copy of a string-like value.
    /// @tparam T  A StringViewable type.
    /// @return    A new std::string with all characters uppered.
    template<StringViewable T>
    std::string to_upper_copy(T &&str) {
        std::string new_str(std::forward<T>(str));
        to_upper(new_str);
        return new_str;
    }

    // ── In-place reversal ─────────────────────────────────────────────────────────

    /// Reverse a mutable character buffer in place.
    /// @tparam T  A MutableCharBuffer type.
    template<MutableCharBuffer T>
    void reverse(T &buf) noexcept {
        std::ranges::reverse(buf);
    }

    /// Return a reversed copy of a string-like value.
    /// @tparam T  A StringViewable type.
    /// @return    A new std::string with the characters in reverse order.
    template<StringViewable T>
    std::string reverse_copy(T &&str) {
        std::string_view sv(std::forward<T>(str));
        return {sv.rbegin(), sv.rend()};
    }

    // ── View-based trimming ───────────────────────────────────────────────────────

    /// Remove leading whitespace from a string view.
    /// @return  A substring view with leading whitespace removed.
    inline std::string_view ltrim(const std::string_view sv) noexcept {
        const auto it = std::ranges::find_if(sv, [](unsigned char ch) noexcept {
            return !std::isspace(ch);
        });
        return sv.substr(static_cast<size_t>(std::distance(sv.begin(), it)));
    }

    /// Remove trailing whitespace from a string view.
    /// @return  A substring view with trailing whitespace removed.
    inline std::string_view rtrim(const std::string_view sv) noexcept {
        const auto it = std::ranges::find_if(
            sv | std::views::reverse,
            [](unsigned char ch) noexcept {
                return !std::isspace(ch);
            }
        );
        return sv.substr(0, static_cast<size_t>(std::distance(sv.begin(), it.base())));
    }

    /// Remove leading and trailing whitespace from a string view.
    /// @return  A substring view with both ends trimmed.
    inline std::string_view trim(const std::string_view sv) noexcept {
        return ltrim(rtrim(sv));
    }

    // ── Copying trimming (convenience wrappers) ───────────────────────────────────

    /// Return a left-trimmed copy of a string-like value.
    /// @tparam T  A StringViewable type.
    template<StringViewable T>
    std::string ltrim_copy(T &&str) {
        return std::string(ltrim(std::string_view(std::forward<T>(str))));
    }

    /// Return a right-trimmed copy of a string-like value.
    /// @tparam T  A StringViewable type.
    template<StringViewable T>
    std::string rtrim_copy(T &&str) {
        return std::string(rtrim(std::string_view(std::forward<T>(str))));
    }

    /// Return a fully trimmed copy of a string-like value.
    /// @tparam T  A StringViewable type.
    template<StringViewable T>
    std::string trim_copy(T &&str) {
        return std::string(trim(std::string_view(std::forward<T>(str))));
    }

    // ── Replace (pair list) ───────────────────────────────────────────────────────

    /// Replace occurrences of multiple target strings in place.
    /// Each target is replaced only once (the first match) per entry in the list.
    /// @tparam R  A PairViewable range (range of {target, replacement} pairs).
    /// @param str          The string to modify in place.
    /// @param replace_list List of {target, replacement} pairs to apply sequentially.
    template<detail::PairViewable R>
    void replace(std::string &str, const R &replace_list) {
        for (const auto &[target, new_content]: replace_list) {
            const std::string_view ts(target);
            const std::string_view rs(new_content);
            if (ts.empty()) continue;

            const auto pos = str.find(ts);
            if (pos == std::string::npos) continue;

            // Equal length: in-place overwrite, no shifting
            if (ts.size() == rs.size()) {
                // Avoid aliasing issues when rs points inside str
                if (rs.data() >= str.data() && rs.data() < str.data() + str.size()) {
                    std::string repl(rs);
                    auto p = pos;
                    do {
                        std::copy_n(repl.data(), rs.size(), str.data() + p);
                        p = str.find(ts, p + rs.size());
                    } while (p != std::string::npos);
                } else {
                    auto p = pos;
                    do {
                        std::copy_n(rs.data(), rs.size(), str.data() + p);
                        p = str.find(ts, p + rs.size());
                    } while (p != std::string::npos);
                }
            } else {
                auto p = pos;
                do {
                    str.replace(p, ts.length(), rs);
                    p = str.find(ts, p + rs.length());
                } while (p != std::string::npos);
            }
        }
    }

    /// Return a copy of the string with the first occurrence of each target replaced.
    /// @tparam R  A PairViewable range.
    /// @param str          The input string to transform.
    /// @param replace_list List of {target, replacement} pairs to apply sequentially.
    /// @return             A new string with replacements applied.
    template<detail::PairViewable R>
    std::string replace_copy(std::string_view str, const R &replace_list) {
        auto s = std::string(str);
        replace(s, replace_list);
        return s;
    }

    // ── Replace (single target/replacement) ───────────────────────────────────────

    /// Replace all occurrences of `target` with `replacement` in place.
    ///
    /// For equal-length strings, performs an in-place overwrite with no allocation.
    /// For unequal-length strings, builds a new string with a single allocation.
    ///
    /// @tparam S          A StringViewable target type.
    /// @tparam T          A StringViewable replacement type.
    /// @param str         The string to modify in place.
    /// @param target      The substring to search for.
    /// @param replacement The string to replace each match with.
    template<StringViewable S, StringViewable T>
    void replace_all(std::string &str, S &&target, T &&replacement) {
        const std::string_view ts(std::forward<S>(target));
        const std::string_view rs(std::forward<T>(replacement));
        if (ts.empty()) return;

        // Equal-length: in-place overwrite, no allocation
        if (ts.size() == rs.size()) {
            if (rs.data() >= str.data() && rs.data() < str.data() + str.size()) {
                std::string repl(rs);
                auto pos = str.find(ts);
                while (pos != std::string::npos) {
                    std::copy_n(repl.data(), rs.size(), str.data() + pos);
                    pos = str.find(ts, pos + rs.size());
                }
            } else {
                auto pos = str.find(ts);
                while (pos != std::string::npos) {
                    std::copy_n(rs.data(), rs.size(), str.data() + pos);
                    pos = str.find(ts, pos + rs.size());
                }
            }
            return;
        }

        // Unequal-length: build new string, single allocation
        std::string result;
        size_t last = 0;
        auto pos = str.find(ts);
        while (pos != std::string::npos) {
            result.append(str, last, pos - last);
            result.append(rs);
            last = pos + ts.size();
            pos = str.find(ts, last);
        }
        result.append(str, last);
        str.swap(result);
    }

    /// Replace the first occurrence of `target` with `replacement` in place.
    /// @tparam S          A StringViewable target type.
    /// @tparam T          A StringViewable replacement type.
    /// @param str         The string to modify in place.
    /// @param target      The substring to search for.
    /// @param replacement The string to replace the first match with.
    /// @return            true if a replacement was made, false if target was not found.
    template<StringViewable S, StringViewable T>
    bool replace_first(std::string &str, S &&target, T &&replacement) {
        const std::string_view ts(std::forward<S>(target));
        const std::string_view rs(std::forward<T>(replacement));
        if (ts.empty()) return false;

        const auto pos = str.find(ts);
        if (pos == std::string::npos) return false;

        str.replace(pos, ts.length(), rs);
        return true;
    }

    /// Return a copy with all occurrences of `target` replaced by `replacement`.
    /// @tparam S          A StringViewable target type.
    /// @tparam T          A StringViewable replacement type.
    /// @param str         The input string to transform.
    /// @param target      The substring to search for.
    /// @param replacement The string to replace each match with.
    /// @return            A new string with all replacements applied.
    template<StringViewable S, StringViewable T>
    std::string replace_all_copy(std::string_view str, S &&target, T &&replacement) {
        auto s = std::string(str);
        replace_all(s, std::forward<S>(target), std::forward<T>(replacement));
        return s;
    }

    // ── Split / Join ──────────────────────────────────────────────────────────────

    /// Splits a string-like value by `delim`. Empty parts are skipped.
    /// Delimiter must not be empty; asserts in debug builds.
    /// @tparam T     A StringViewable type.
    /// @param str    The string to split.
    /// @param delim  The delimiter string (default: single space).
    /// @return       A vector of non-empty substrings.
    template<StringViewable T>
    std::vector<std::string> split(T &&str, const std::string_view delim = " ") {
        if (delim.empty()) {
            throw std::invalid_argument("delimiter must not be empty");
        }
        const std::string_view sv(std::forward<T>(str));
        std::vector<std::string> output;

        if (!sv.empty()) {
            output.reserve(sv.size() / 8 + 1);
        }

        for (auto part: sv | std::views::split(delim)) {
            if (!part.empty()) {
                output.emplace_back(part.begin(), part.end());
            }
        }

        return output;
    }

    /// Joins a range of string-like values with `delim` between them.
    /// Constraint strengthened to forward_range to allow two-pass algorithm.
    /// @tparam R     A forward range whose value type is StringViewable.
    /// @param parts  The range of string-like values to join.
    /// @param delim  The delimiter to insert between elements.
    /// @return       A concatenated string with delimiters.
    template<std::ranges::forward_range R> requires StringViewable<std::ranges::range_value_t<R> >
    std::string join(R &&parts, const std::string_view delim) {
        size_t total = 0;
        size_t count = 0;
        for (const auto &part: parts) {
            if (count++ > 0) total += delim.size();
            total += std::string_view(part).size();
        }

        std::string result;
        result.reserve(total);

        bool first = true;
        for (const auto &part: parts) {
            if (!first) result += delim;
            result.append(std::string_view(part));
            first = false;
        }

        return result;
    }

    // ── Query functions ───────────────────────────────────────────────────────────

    /// Check if a string contains a substring.
    /// @tparam T  A StringViewable type (the haystack).
    /// @tparam U  A StringViewable type (the needle).
    /// @param str  The string to search in.
    /// @param sub  The substring to search for.
    /// @return     true if `sub` is found within `str`.
    template<StringViewable T, StringViewable U>
    constexpr bool contains(T &&str, U &&sub) noexcept {
        return std::string_view(std::forward<T>(str))
               .find(std::string_view(std::forward<U>(sub))) != std::string_view::npos;
    }

    /// Count the number of (non-overlapping) occurrences of `sub` in `str`.
    /// An empty `sub` returns 0.
    /// @tparam T  A StringViewable type (the haystack).
    /// @tparam U  A StringViewable type (the needle).
    /// @param str  The string to search in.
    /// @param sub  The substring to count.
    /// @return     The number of occurrences.
    template<StringViewable T, StringViewable U>
    constexpr size_t count(T &&str, U &&sub) noexcept {
        const std::string_view sv(std::forward<T>(str));
        const std::string_view sub_sv(std::forward<U>(sub));
        if (sub_sv.empty()) return 0;

        size_t n = 0;
        auto pos = sv.find(sub_sv);
        while (pos != std::string_view::npos) {
            ++n;
            pos = sv.find(sub_sv, pos + sub_sv.size());
        }
        return n;
    }

    // ── String → bool ─────────────────────────────────────────────────────────────

    /// Convert a string representation of a boolean to bool.
    /// Recognises "1", "on"/"ON", "yes"/"YES", "true"/"TRUE" as true;
    /// everything else is false.
    /// @tparam T  A StringViewable type.
    /// @return     The boolean value.
    template<StringViewable T>
    constexpr bool str_to_bool(T &&str) noexcept {
        const std::string_view s(std::forward<T>(str));
        return s == "1" || s == "on" || s == "ON" ||
               s == "yes" || s == "YES" ||
               s == "true" || s == "TRUE";
    }
} // namespace StringUtil

#endif // YADDNSC_STRING_UTIL_H
