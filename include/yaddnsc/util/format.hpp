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

/// Replace placeholders in the original template only; inserted values are literal.
/// {{ and }} escape braces. Unknown/unclosed placeholders are preserved.
/// For duplicate names the last argument wins.
[[nodiscard]] inline std::string format_named(std::string_view fmt_,
                                              const std::unordered_map<std::string, std::string>& values) {
    std::string result;
    result.reserve(fmt_.size());
    for (std::size_t pos = 0; pos < fmt_.size();) {
        if (fmt_.substr(pos, 2) == "{{" || fmt_.substr(pos, 2) == "}}") {
            result += fmt_[pos];
            pos += 2;
        } else if (fmt_[pos] == '{') {
            const auto end = fmt_.find('}', pos + 1);
            if (end == std::string_view::npos) {
                result.append(fmt_.substr(pos));
                break;
            }
            const auto found = values.find(std::string(fmt_.substr(pos + 1, end - pos - 1)));
            if (found == values.end()) {
                result.append(fmt_.substr(pos, end - pos + 1));
            } else {
                result.append(found->second);
            }
            pos = end + 1;
        } else {
            result += fmt_[pos++];
        }
    }
    return result;
}

namespace detail {

/// Store one named argument. The name is read before the value is moved out
/// of the argument: C++17 sequences the right operand of an assignment first,
/// so moving inside the assignment would read the name after the move.
template<typename Arg>
void store_named(std::unordered_map<std::string, std::string>& values, Arg&& arg) {
    const std::string name{arg.name};
    values[name] = std::forward<Arg>(arg).value;
}

}  // namespace detail

/// Named arguments use format_named; the last duplicate name wins.
template<typename First, typename... Rest>
    requires std::same_as<std::decay_t<First>, NamedArg> && (std::same_as<std::decay_t<Rest>, NamedArg> && ...)
std::string format(std::string_view fmt_, First&& first, Rest&&... rest) {
    std::unordered_map<std::string, std::string> m;
    detail::store_named(m, std::forward<First>(first));
    (detail::store_named(m, std::forward<Rest>(rest)), ...);
    return format_named(fmt_, m);
}

}  // namespace yaddnsc::util::fmt

#endif  // YADDNSC_UTIL_FORMAT_HPP
