//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_FMT_H
#define YADDNSC_FMT_H

#include <map>
#include <string>
#include <vector>
#include <string_view>
#include <type_traits>
#include <concepts>

// ---------------------------------------------------------------------------
// Polyfill: bridges std::format and the external fmt library so that the rest
// of the codebase can always use `fmt::format(...)`, `fmt::join(...)`, etc.
// ---------------------------------------------------------------------------

#ifdef YADDNSC_USE_STD_FORMAT

// ---- native std::format (C++20 / C++23) -----------------------------------

#include <format>
#include <ranges>
#include <algorithm>

namespace fmt {
    // --- named_arg type -----------------------------------------------------

    struct named_arg_t {
        std::string_view name;
        std::string value;

        // Convert any value to string for storage
        template<typename T>
        static named_arg_t create(std::string_view name, T &&val) {
            if constexpr (std::constructible_from<std::string, T>) {
                return {name, std::string(std::forward<T>(val))};
            } else {
                return {name, std::format("{}", std::forward<T>(val))};
            }
        }
    };

    // --- arg() API (identical to the real fmt library) ---------------------

    template<typename T>
    named_arg_t arg(std::string_view name, T &&value) {
        return named_arg_t::create(name, std::forward<T>(value));
    }

    // --- custom vformat overloads (no std equivalents) ----------------------

    // Named: replaces {KEY} with the corresponding map value.
    inline std::string vformat(std::string_view fmt_, const std::map<std::string, std::string> &args) {
        std::string result(fmt_);
        for (const auto &[key, val]: args) {
            auto placeholder = std::string("{") + key + "}";
            auto pos = result.find(placeholder);
            if (pos != std::string::npos) {
                result.replace(pos, placeholder.length(), val);
            }
        }
        return result;
    }

    // Positional: replaces {0}, {1}, … with elements from a vector.
    inline std::string vformat(std::string_view fmt_, const std::vector<std::string_view> &args) {
        std::string result(fmt_);
        for (size_t i = 0; i < args.size(); ++i) {
            auto placeholder = std::string("{") + std::to_string(i) + "}";
            auto pos = result.find(placeholder);
            if (pos != std::string::npos) {
                result.replace(pos, placeholder.length(), args[i]);
            }
        }
        return result;
    }

    // --- basic formatting ---------------------------------------------------

    // For regular (positional) arguments: delegate to std::format.
    template<typename... Args>
        requires (!(std::same_as<std::decay_t<Args>, named_arg_t> || ...))
    auto format(std::format_string<Args...> fmt, Args &&... args)
        -> decltype(std::format(fmt, std::forward<Args>(args)...)) {
        return std::format(fmt, std::forward<Args>(args)...);
    }

    // For named arguments (at least one): collect into a map and call vformat(map).
    template<typename First, typename... Rest>
        requires std::same_as<std::decay_t<First>, named_arg_t> &&
                 (std::same_as<std::decay_t<Rest>, named_arg_t> && ...)
    std::string format(std::string_view fmt_, First &&first, Rest &&... rest) {
        std::map<std::string, std::string> m;
        m[std::forward<First>(first).name.data()] = std::forward<First>(first).value;
        ((m[std::forward<Rest>(rest).name.data()] =
          std::forward<Rest>(rest).value),
            ...);
        return vformat(fmt_, m);
    }

    inline std::string vformat(std::string_view fmt_, std::format_args args) {
        return std::vformat(fmt_, args);
    }

    using std::format_to;
    using std::format_context;
    using std::format_parse_context;
    using std::basic_format_args;
    using std::format_args;

    template<typename T>
    using formatter = std::formatter<T>;

    template<typename... Args>
    auto make_format_args(Args &&... args) {
        return std::make_format_args(std::forward<Args>(args)...);
    }

    // --- join (no std equivalent) -------------------------------------------

    template<std::ranges::input_range Range>
    std::string join(Range &&range, std::string_view sep) {
        std::string result;
        bool first = true;
        for (const auto &item: range) {
            if (!first) {
                result += sep;
            }
            // Convert whatever the range yields to a string_view.
            using std::data;
            using std::size;
            result.append(data(item), size(item));
            first = false;
        }
        return result;
    }
} // namespace fmt

#else // !YADDNSC_USE_STD_FORMAT – use the real fmt library

// ---- external fmt library --------------------------------------------------

#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/args.h>
#include <fmt/ranges.h>

#endif // YADDNSC_USE_STD_FORMAT

#endif // YADDNSC_FMT_H
