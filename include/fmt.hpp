//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_FMT_H
#define YADDNSC_FMT_H

/// Polyfill: bridges std::format and the external fmt library so that the rest
/// of the codebase can always use `fmt::format(...)`, `fmt::join(...)`, etc.

#ifdef YADDNSC_USE_STD_FORMAT

// ---- native std::format (C++20 / C++23) -----------------------------------

#include <format>
#include <ranges>
#include <concepts>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace fmt {
    /// A named argument for fmt::format, stored as a string value.
    struct named_arg_t {
        std::string_view name;  ///< Argument name (used in format string as {name})
        std::string value;      ///< Stringified argument value

        /// Convert any value to string for storage.
        template<typename T>
        static named_arg_t create(std::string_view name, T &&val) {
            if constexpr (std::constructible_from<std::string, T>) {
                return {name, std::string(std::forward<T>(val))};
            } else {
                return {name, std::format("{}", std::forward<T>(val))};
            }
        }
    };

    /// Create a named argument (identical to the real fmt library API).
    template<typename T>
    named_arg_t arg(std::string_view name, T &&value) {
        return named_arg_t::create(name, std::forward<T>(value));
    }

    // --- basic formatting ---------------------------------------------------

    /// Format with positional arguments — delegates to std::format.
    template<typename... Args>
        requires (!(std::same_as<std::decay_t<Args>, named_arg_t> || ...))
    std::string format(std::format_string<Args...> fmt, Args &&... args) {
        return std::format(fmt, std::forward<Args>(args)...);
    }

    /// Format with named arguments — replaces {KEY} with corresponding values.
    template<typename First, typename... Rest>
        requires std::same_as<std::decay_t<First>, named_arg_t> &&
                 (std::same_as<std::decay_t<Rest>, named_arg_t> && ...)
    std::string format(std::string_view fmt_, First &&first, Rest &&... rest) {
        std::unordered_map<std::string, std::string> m;
        m[std::string(first.name)] = std::move(first.value);
        ((m[std::string(rest.name)] = std::move(rest.value)), ...);
        std::string result(fmt_);
        for (const auto &[key, val]: m) {
            auto ph = std::string("{") + key + "}";
            for (auto pos = result.find(ph); pos != std::string::npos;
                 pos = result.find(ph, pos + val.size())) {
                result.replace(pos, ph.length(), val);
            }
        }
        return result;
    }

    using std::vformat;
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

    /// Join a range of elements with a separator (no std equivalent in C++20).
    template<std::ranges::input_range Range>
    std::string join(Range &&range, std::string_view sep) {
        std::string result;
        bool first = true;
        for (const auto &item: range) {
            if (!first) {
                result += sep;
            }
            result.append(std::data(item), std::size(item));
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
