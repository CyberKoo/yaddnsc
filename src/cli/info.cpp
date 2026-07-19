//
// Created by Kotarou on 2026/7/6.
//

#include "info.h"

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <print>

#include "min_update_interval.h"
#include "resolver_config.h"
#include "version.h"
#include "build_id.hpp"



namespace Cli {
    void register_info_subcommand(CLI::App &app, int &exit_code) {
        auto *info = app.add_subcommand("info", "Show build configuration");
        info->callback([&exit_code] {
            std::println("Build configuration:");
            std::println("  {:<20} {}", "Version:", YADDNSC::get_full_version());
            std::println("  {:<20} {}", "Build ID:", BuildId::full_id());
            std::println("  {:<20} {}", "C library:", BuildId::LIBC_TYPE);
            if (BuildId::GLIBCXX_CXX11_ABI) {
                std::println("  {:<20} {} (_GLIBCXX_USE_CXX11_ABI=1)", "Compiler ABI:", BuildId::COMPILER_ABI);
            } else {
                std::println("  {:<20} {}", "Compiler ABI:", BuildId::COMPILER_ABI);
            }
            std::println("  {:<20} 0x{:016X}", "Compiler ID hash:", BuildId::COMPILER_ID_HASH);

            std::println("  {:<20} C++{}", "C++ standard:", __cplusplus / 100 % 100);

            std::println("  {:<20} {}", "DNS resolver:", YADDNSC_USE_NATIVE_DNS ? "Native" : "System (libresolv)");

            std::println("  {:<20} {}:{}", "Default DNS:", YADDNSC_DEFAULT_DNS_SERVER, YADDNSC_DEFAULT_DNS_PORT);

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
