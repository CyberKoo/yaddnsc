//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_UTIL_FORMAT_HPP
#define YADDNSC_UTIL_FORMAT_HPP

/// Formatting helpers — the single implementation of the named-argument
/// replacement mechanism ({key} -> value) shared by the host's fmt polyfill
/// (src/support/fmt.hpp) and driver plugins (yaddnsc/sdk/format.hpp).

#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <concepts>

#include <format>

namespace yaddnsc::util::fmt {

/// A named argument, stored as a string value.
struct NamedArg {
    std::string_view name;
    std::string value;

    template<typename T>
    static NamedArg create(std::string_view name, T&& val) {
        if constexpr (std::constructible_from<std::string, T> && !std::is_arithmetic_v<std::decay_t<T>>) {
            return {name, std::string(std::forward<T>(val))};
        } else {
            return {name, std::format("{}", std::forward<T>(val))};
        }
    }
};

/// Create a named argument.
template<typename T>
NamedArg arg(std::string_view name, T&& value) {
    return NamedArg::create(name, std::forward<T>(value));
}

/// Format with positional arguments — delegates to std::format.
template<typename... Args>
    requires(!(std::same_as<std::decay_t<Args>, NamedArg> || ...))
std::string format(std::format_string<Args...> fmt, Args&&... args) {
    return std::format(fmt, std::forward<Args>(args)...);
}

/// Format with named arguments — replaces {KEY} with corresponding values.
template<typename First, typename... Rest>
    requires std::same_as<std::decay_t<First>, NamedArg> && (std::same_as<std::decay_t<Rest>, NamedArg> && ...)
std::string format(std::string_view fmt_, First&& first, Rest&&... rest) {
    std::unordered_map<std::string, std::string> m;
    m[std::string(first.name)] = std::forward<First>(first).value;
    ((m[std::string(rest.name)] = std::forward<Rest>(rest).value), ...);
    std::string result(fmt_);
    for (const auto& [key, val] : m) {
        auto ph = std::string("{") + key + "}";
        for (auto pos = result.find(ph); pos != std::string::npos; pos = result.find(ph, pos + val.size())) {
            result.replace(pos, ph.length(), val);
        }
    }
    return result;
}

}  // namespace yaddnsc::util::fmt

#endif  // YADDNSC_UTIL_FORMAT_HPP
