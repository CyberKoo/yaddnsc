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
#include <cassert>

namespace StringUtil {
    // ── Concepts ──────────────────────────────────────────────────────────────────

    /// Any contiguous char buffer that can be mutated (std::string, std::pmr::string, std::vector<char>, etc.)
    template<typename T>
    concept MutableCharBuffer = std::ranges::contiguous_range<T> && std::same_as<std::ranges::range_value_t<T>, char>;

    /// Any type that can be viewed as a string_view (const char*, std::string, std::string_view, etc.)
    template<typename T>
    concept StringViewable = std::is_constructible_v<std::string_view, const T &>;

    // ── Implementation details ────────────────────────────────────────────────────

    namespace detail {
        // Single definition to avoid repeated instantiation in each template
        inline constexpr std::array<char, 256> lower_table = []() noexcept {
            std::array<char, 256> table{};
            for (int i = 0; i < 256; ++i)
                table[i] = (i >= 'A' && i <= 'Z') ? static_cast<char>(i + 32) : static_cast<char>(i);
            return table;
        }();

        inline constexpr std::array<char, 256> upper_table = []() noexcept {
            std::array<char, 256> table{};
            for (int i = 0; i < 256; ++i)
                table[i] = (i >= 'a' && i <= 'z') ? static_cast<char>(i - 32) : static_cast<char>(i);
            return table;
        }();

        // Fixed: check member access `v.first` / `v.second` and string_view constructibility
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

    template<MutableCharBuffer T>
    void to_lower(T &buf) noexcept {
        std::ranges::transform(buf, buf.begin(), [&](char c) noexcept {
            return detail::lower_table[static_cast<uint8_t>(c)];
        });
    }

    template<MutableCharBuffer T>
    void to_upper(T &buf) noexcept {
        std::ranges::transform(buf, buf.begin(), [&](char c) noexcept {
            return detail::upper_table[static_cast<uint8_t>(c)];
        });
    }

    // ── Copying case conversion ───────────────────────────────────────────────────

    template<StringViewable T>
    std::string to_lower_copy(T &&str) {
        std::string new_str(std::forward<T>(str));
        to_lower(new_str);
        return new_str;
    }

    template<StringViewable T>
    std::string to_upper_copy(T &&str) {
        std::string new_str(std::forward<T>(str));
        to_upper(new_str);
        return new_str;
    }

    // ── In-place reversal ─────────────────────────────────────────────────────────

    template<MutableCharBuffer T>
    void reverse(T &buf) noexcept {
        std::ranges::reverse(buf);
    }

    template<StringViewable T>
    std::string reverse_copy(T &&str) {
        std::string_view sv(std::forward<T>(str));
        return {sv.rbegin(), sv.rend()};
    }

    // ── View-based trimming ───────────────────────────────────────────────────────

    inline std::string_view ltrim(const std::string_view sv) noexcept {
        const auto it = std::ranges::find_if(sv, [](unsigned char ch) noexcept {
            return !std::isspace(ch);
        });
        return sv.substr(static_cast<size_t>(std::distance(sv.begin(), it)));
    }

    inline std::string_view rtrim(const std::string_view sv) noexcept {
        const auto it = std::ranges::find_if(
            sv | std::views::reverse,
            [](unsigned char ch) noexcept {
                return !std::isspace(ch);
            }
        );
        return sv.substr(0, static_cast<size_t>(std::distance(sv.begin(), it.base())));
    }

    inline std::string_view trim(const std::string_view sv) noexcept {
        return ltrim(rtrim(sv));
    }

    // ── Copying trimming (convenience wrappers) ───────────────────────────────────

    template<StringViewable T>
    std::string ltrim_copy(T &&str) {
        return std::string(ltrim(std::string_view(std::forward<T>(str))));
    }

    template<StringViewable T>
    std::string rtrim_copy(T &&str) {
        return std::string(rtrim(std::string_view(std::forward<T>(str))));
    }

    template<StringViewable T>
    std::string trim_copy(T &&str) {
        return std::string(trim(std::string_view(std::forward<T>(str))));
    }

    // ── Replace (pair list) ───────────────────────────────────────────────────────

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

    template<detail::PairViewable R>
    std::string replace_copy(std::string_view str, const R &replace_list) {
        auto s = std::string(str);
        replace(s, replace_list);
        return s;
    }

    // ── Replace (single target/replacement) ───────────────────────────────────────

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

    template<StringViewable S, StringViewable T>
    std::string replace_all_copy(std::string_view str, S &&target, T &&replacement) {
        auto s = std::string(str);
        replace_all(s, std::forward<S>(target), std::forward<T>(replacement));
        return s;
    }

    // ── Split / Join ──────────────────────────────────────────────────────────────

    /// Splits a string-like value by `delim`. Empty parts are skipped.
    /// Delimiter must not be empty; asserts in debug builds.
    template<StringViewable T>
    std::vector<std::string> split(T &&str, const std::string_view delim = " ") {
        assert(!delim.empty() && "delimiter must not be empty"); // undefined behavior with views::split
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

    template<StringViewable T, StringViewable U>
    constexpr bool contains(T &&str, U &&sub) noexcept {
        return std::string_view(std::forward<T>(str))
               .find(std::string_view(std::forward<U>(sub))) != std::string_view::npos;
    }

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

    template<StringViewable T>
    constexpr bool str_to_bool(T &&str) noexcept {
        const std::string_view s(std::forward<T>(str));
        return s == "1" || s == "on" || s == "ON" ||
               s == "yes" || s == "YES" ||
               s == "true" || s == "TRUE";
    }
} // namespace StringUtil

#endif // YADDNSC_STRING_UTIL_H
