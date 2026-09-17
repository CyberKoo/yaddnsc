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
#include <string>
#include <string_view>

#include <yaddnsc/util/format.hpp>

namespace fmt {
    // Named-argument machinery: single implementation shared with the plugin
    // SDK (yaddnsc/util/format.hpp).
    using yaddnsc::util::fmt::named_arg_t;
    using yaddnsc::util::fmt::arg;
    using yaddnsc::util::fmt::format;

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
