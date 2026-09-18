# ==============================================================================
# Include-What-You-Use (include hygiene)
# ==============================================================================
# Enforces IWYU during compilation: every translation unit is analyzed and
# violations fail the build (-Xiwyu --error). The resulting command is
# consumed by the top-level CMakeLists.txt AFTER third-party dependencies are
# created, so CPM packages are never analyzed.
#
# Only enabled with upstream Clang: IWYU embeds a Clang frontend that rejects
# GCC's module-scanning flags and trips over some libstdc++ internals. Do not
# run the Homebrew IWYU binary with AppleClang: its embedded frontend need not
# match the Xcode compiler or macOS SDK.
option(YADDNSC_IWYU "Enforce include-what-you-use during compilation" ON)

if(NOT YADDNSC_IWYU)
    return()
endif()

find_program(IWYU_EXE NAMES include-what-you-use iwyu)

if(NOT IWYU_EXE)
    message(STATUS "IWYU not found — include hygiene checks skipped.")
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(STATUS "IWYU skipped (compiler is ${CMAKE_CXX_COMPILER_ID}, requires upstream Clang)")
    return()
endif()

message(STATUS "IWYU found: ${IWYU_EXE} (violations fail the build)")
set(YADDNSC_IWYU_COMMAND
    "${IWYU_EXE};-Xiwyu;--error;-Xiwyu;--mapping_file=${CMAKE_SOURCE_DIR}/.iwyu-mappings.imp")
