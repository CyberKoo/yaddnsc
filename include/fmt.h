//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_FMT_H
#define YADDNSC_FMT_H

#include <map>
#include <string>
#include <vector>
#include <string_view>

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

    // --- basic formatting ---------------------------------------------------

    using std::format;

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

    // --- custom vformat overloads (no std equivalents) ----------------------

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
