//
// Created by Kotarou on 2026/7/6.
//

#include "info.h"

#include <cstdlib>
#include <print>

#include "version.h"
#include "resolver_config.h"
#include "min_update_interval.h"

namespace Cli {
    void register_info_subcommand(CLI::App &app, int &exit_code) {
        auto *info = app.add_subcommand("info", "Show build configuration");
        info->callback([&exit_code] {
            std::println("Build configuration:");
            std::println("  {:<20} {}", "Version:", yaddnsc::get_full_version());

#if defined(NDEBUG)
            std::println("  {:<20} {}", "Build type:", "Release");
#else
            std::println("  {:<20} {}", "Build type:", "Debug");
#endif

#if defined(__clang__)
            std::println("  {:<20} Clang {}.{}.{}", "Compiler:", __clang_major__,
                         __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__) || defined(__GNUG__)
            std::println("  {:<20} GCC {}.{}.{}", "Compiler:", __GNUC__,
                         __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
            std::println("  {:<20} {}", "Compiler:", _MSC_VER);
#else
            std::println("  {:<20} {}", "Compiler:", "Unknown");
#endif

            std::println("  {:<20} C++{}", "C++ standard:", __cplusplus / 100 % 100);

            std::println("  {:<20} {}", "DNS resolver:",
                         YADDNSC_NATIVE_DNS ? "Native" : "System (libresolv)");

            std::println("  {:<20} {}:{}", "Default DNS:",
                         YADDNSC_DEFAULT_DNS_SERVER, YADDNSC_DEFAULT_DNS_PORT);

            std::println("  {:<20} {}s", "Min update interval:", YADDNSC_MIN_UPDATE_INTERVAL);

#ifdef YADDNSC_USE_STD_FORMAT
            std::println("  {:<20} {}", "Format library:", "std::format");
#else
            std::println("  {:<20} {}", "Format library:", "fmt");
#endif

#ifdef YADDNSC_USE_SYSTEM_SPDLOG
            std::println("  {:<20} {}", "spdlog:", "system package");
#else
            std::println("  {:<20} {}", "spdlog:", "bundled");
#endif

            exit_code = EXIT_SUCCESS;
        });
    }
} // namespace Cli
