//
// Created by Kotarou on 2026/6/17.
//

#include "interface/core_logger.h"

#include <spdlog/spdlog.h>

namespace CoreLogger {
    void trace_impl(std::string_view msg, const char *file, int line, const char *func) {
        spdlog::log(spdlog::source_loc{file, line, func}, spdlog::level::trace, "{}", msg);
    }

    void debug_impl(std::string_view msg, const char *file, int line, const char *func) {
        spdlog::log(spdlog::source_loc{file, line, func}, spdlog::level::debug, "{}", msg);
    }

    void info_impl(std::string_view msg, const char *file, int line, const char *func) {
        spdlog::log(spdlog::source_loc{file, line, func}, spdlog::level::info, "{}", msg);
    }

    void warn_impl(std::string_view msg, const char *file, int line, const char *func) {
        spdlog::log(spdlog::source_loc{file, line, func}, spdlog::level::warn, "{}", msg);
    }

    void error_impl(std::string_view msg, const char *file, int line, const char *func) {
        spdlog::log(spdlog::source_loc{file, line, func}, spdlog::level::err, "{}", msg);
    }
}
