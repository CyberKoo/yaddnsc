#ifndef YADDNSC_SDK_LOG_HPP
#define YADDNSC_SDK_LOG_HPP

/// Lazy Host-Services logging for driver plugins, in both execution modes.
///
/// The mechanism — level query, target resolution, formatting and the host
/// callback — lives here once; the two macro families differ only in the
/// services table they resolve. Both mode headers include this file, so plugin
/// sources keep their existing include path and macro names.

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <yaddnsc/sdk/driver_common.hpp>

namespace yaddnsc::sdk::detail {

using LogEnabledFn = int32_t (*)(void*, yaddnsc_log_level);

template<typename Table>
struct BasicLogTarget {
    const Table* services;
    LogEnabledFn log_enabled;
};

using LogTarget = BasicLogTarget<yaddnsc_host_services>;
using AsyncLogTarget = BasicLogTarget<yaddnsc_async_services>;

[[nodiscard]] inline LogTarget to_log_target(const yaddnsc_host_services* services) noexcept {
    return {services, nullptr};  // Never probe an unvalidated sync extension tail.
}

[[nodiscard]] inline AsyncLogTarget to_log_target(const yaddnsc_async_services* services) noexcept {
    return {services, services != nullptr ? services->log_enabled : nullptr};
}

[[nodiscard]] inline LogTarget to_log_target(std::nullptr_t) noexcept {
    return {nullptr, nullptr};
}

template<typename Services>
    requires(!std::is_pointer_v<Services> && !std::is_null_pointer_v<Services>)
[[nodiscard]] inline auto to_log_target(const Services& services) noexcept {
    if constexpr (requires { services.services(); })
        return to_log_target(services.services());
    else
        return BasicLogTarget{services.get(), services.log_enabled_fn()};
}

template<typename Table, typename... Args>
inline void log_message(const Table* services, yaddnsc_log_level level, std::string_view file, int32_t line,
                        std::string_view function, std::format_string<Args...> format, Args&&... args) noexcept {
    if (services == nullptr || services->log == nullptr)
        return;
    try {
        const std::string message = fmt::format(format, std::forward<Args>(args)...);
        const yaddnsc_source_location location{make_view(file), line, make_view(function)};
        services->log(services->context, level, make_view(message), &location);
    } catch (...) {
    }  // Best-effort logging never fails a business operation.
}

template<typename GetLevel, typename GetServices, typename Log>
inline void log_lazy(GetLevel&& get_level, GetServices&& get_services, Log&& log) noexcept {
    try {
        const auto target = to_log_target(get_services());
        if (target.services == nullptr || target.services->log == nullptr)
            return;
        const yaddnsc_log_level level = get_level();
        if (target.log_enabled != nullptr) {
            bool enabled = true;
            try {
                enabled = target.log_enabled(target.services->context, level) != 0;
            } catch (...) {
                enabled = true;
            }
            if (!enabled)
                return;
        }
        log(target.services, level);
    } catch (...) {
    }  // Includes lookup, argument evaluation and formatting.
}

template<typename... Args>
inline void async_log_message(const yaddnsc_async_services* services, yaddnsc_log_level level, std::string_view file,
                              int32_t line, std::string_view function, std::format_string<Args...> fmt,
                              Args&&... args) noexcept {
    log_message(services, level, file, line, function, fmt, std::forward<Args>(args)...);
}

template<typename GetLevel, typename GetServices, typename Log>
inline void async_log_lazy(GetLevel&& get_level, GetServices&& get_services, Log&& log) noexcept {
    log_lazy(
        std::forward<GetLevel>(get_level),
        [&] {
            if constexpr (std::is_null_pointer_v<std::remove_cvref_t<decltype(get_services())>>) {
                return static_cast<const yaddnsc_async_services*>(get_services());
            } else {
                return get_services();
            }
        },
        std::forward<Log>(log));
}

}  // namespace yaddnsc::sdk::detail

#define YADDNSC_SDK_LOG(svc, level, ...)                                                                               \
    [&](const char* yaddnsc_log_function) {                                                                            \
        ::yaddnsc::sdk::detail::log_lazy(                                                                              \
            [&]() { return static_cast<yaddnsc_log_level>(level); }, [&]() { return (svc); },                          \
            [&](const yaddnsc_host_services* yaddnsc_log_services, yaddnsc_log_level yaddnsc_log_level_value) {        \
                ::yaddnsc::sdk::detail::log_message(yaddnsc_log_services, yaddnsc_log_level_value, __FILE__, __LINE__, \
                                                    yaddnsc_log_function, __VA_ARGS__);                                \
            });                                                                                                        \
    }(__FUNCTION__)
#define YADDNSC_SDK_LOG_TRACE(svc, ...) YADDNSC_SDK_LOG(svc, YADDNSC_LOG_TRACE, __VA_ARGS__)
#define YADDNSC_SDK_LOG_DEBUG(svc, ...) YADDNSC_SDK_LOG(svc, YADDNSC_LOG_DEBUG, __VA_ARGS__)
#define YADDNSC_SDK_LOG_INFO(svc, ...) YADDNSC_SDK_LOG(svc, YADDNSC_LOG_INFO, __VA_ARGS__)
#define YADDNSC_SDK_LOG_WARN(svc, ...) YADDNSC_SDK_LOG(svc, YADDNSC_LOG_WARN, __VA_ARGS__)
#define YADDNSC_SDK_LOG_ERROR(svc, ...) YADDNSC_SDK_LOG(svc, YADDNSC_LOG_ERROR, __VA_ARGS__)

#define YADDNSC_ASYNC_SDK_LOG(svc, level, ...)                                                                     \
    [&](const char* yaddnsc_log_function) {                                                                        \
        ::yaddnsc::sdk::detail::async_log_lazy(                                                                    \
            [&]() { return static_cast<yaddnsc_log_level>(level); }, [&]() { return (svc); },                      \
            [&](const yaddnsc_async_services* yaddnsc_log_services, yaddnsc_log_level yaddnsc_log_level_value) {   \
                ::yaddnsc::sdk::detail::async_log_message(yaddnsc_log_services, yaddnsc_log_level_value, __FILE__, \
                                                          __LINE__, yaddnsc_log_function, __VA_ARGS__);            \
            });                                                                                                    \
    }(__FUNCTION__)
#define YADDNSC_ASYNC_SDK_LOG_TRACE(svc, ...) YADDNSC_ASYNC_SDK_LOG(svc, YADDNSC_LOG_TRACE, __VA_ARGS__)
#define YADDNSC_ASYNC_SDK_LOG_DEBUG(svc, ...) YADDNSC_ASYNC_SDK_LOG(svc, YADDNSC_LOG_DEBUG, __VA_ARGS__)
#define YADDNSC_ASYNC_SDK_LOG_INFO(svc, ...) YADDNSC_ASYNC_SDK_LOG(svc, YADDNSC_LOG_INFO, __VA_ARGS__)
#define YADDNSC_ASYNC_SDK_LOG_WARN(svc, ...) YADDNSC_ASYNC_SDK_LOG(svc, YADDNSC_LOG_WARN, __VA_ARGS__)
#define YADDNSC_ASYNC_SDK_LOG_ERROR(svc, ...) YADDNSC_ASYNC_SDK_LOG(svc, YADDNSC_LOG_ERROR, __VA_ARGS__)

#endif  // YADDNSC_SDK_LOG_HPP
