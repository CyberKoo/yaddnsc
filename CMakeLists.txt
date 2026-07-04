cmake_minimum_required(VERSION 3.28...3.31 FATAL_ERROR)
project(yaddnsc VERSION 1.0.0)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# ==============================================================================
# Compiler & project-wide settings
# ==============================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# default to release if not specified
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

# ==============================================================================
# Compiler version requirements
# ==============================================================================
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "14")
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old. yaddnsc requires GCC 14+ (C++23).")
  endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "18")
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old. yaddnsc requires Clang 18+ (C++23).")
  endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "15")
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old. yaddnsc requires AppleClang 15+ (C++23).")
  endif ()
else ()
  message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. yaddnsc requires GCC 14+, Clang 18+, or AppleClang 15+.")
endif ()

# ==============================================================================
# Feature detection
# ==============================================================================
include(FeatureCheck)

# ==============================================================================
# Release build flags
# ==============================================================================
# On musl, res_mkquery / libresolv is not available (musl's resolv.h is
# a stub), so the manual DNS query builder must be used by default.
if (LIBC_MUSL AND NOT DEFINED YADDNSC_MANUAL_MKQUERY)
  set(YADDNSC_MANUAL_MKQUERY ON CACHE BOOL
      "Use manual DNS query builder instead of system res_mkquery" FORCE)
  message(STATUS "musl detected — YADDNSC_MANUAL_MKQUERY defaulting to ON")
endif ()

# ==============================================================================
# Version info
# ==============================================================================
# Version is now managed by project(yaddnsc VERSION 1.0.0).
# The version.h.in template uses @PROJECT_VERSION_MAJOR@ etc. directly,
# so no YADDNSC_VERSION_* aliases are needed here.
# ABI version — semantic version for driver compatibility.
#   major — breaking ABI change (virtual function removed or signature changed)
#   minor — backward-compatible ABI extension (new virtual function appended)
#   patch — no ABI change, only implementation changes
set(YADDNSC_ABI_VERSION_MAJOR 1)
set(YADDNSC_ABI_VERSION_MINOR 0)
set(YADDNSC_ABI_VERSION_PATCH 0)

set(YADDNSC_DEVELOPMENT ON)
set(YADDNSC_RELEASE_INFO RELEASE)

if (YADDNSC_DEVELOPMENT)
  include(GitVersion)
  set(YADDNSC_RELEASE_INFO ${GIT_VERSION})
endif ()

# ==============================================================================
# User options (cache variables, overridable via -D)
# ==============================================================================
set(YADDNSC_LOGGING_PATTERN "[%D %T.%e] [%^%8l%$] [%8!t] [%15!s:%-4#] %v" CACHE STRING
        "Logging pattern passed to spdlog::set_pattern()")

set(YADDNSC_MIN_UPDATE_INTERVAL 60 CACHE STRING "Minimum allowed update interval (seconds)")

option(YADDNSC_MANUAL_MKQUERY "Use manual DNS query builder instead of system res_mkquery" OFF)
option(YADDNSC_USE_SYSTEM_SPDLOG "Use spdlog from the system instead of the bundled CPM version" OFF)

if (YADDNSC_MIN_UPDATE_INTERVAL LESS 0)
  message(FATAL_ERROR "YADDNSC_MIN_UPDATE_INTERVAL must not be negative, got ${YADDNSC_MIN_UPDATE_INTERVAL}")
endif ()

# ==============================================================================
# Generated headers (all configure_file calls, after all variables are defined)
# ==============================================================================
configure_file("${CMAKE_SOURCE_DIR}/src/template/version.h.in" "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/src/template/driver_ver.h.in" "${CMAKE_BINARY_DIR}/generated/driver_ver.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/src/template/config_cmake.h.in" "${CMAKE_BINARY_DIR}/generated/config_cmake.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/src/template/logging_pattern.h.in" "${CMAKE_BINARY_DIR}/generated/logging_pattern.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/src/template/min_update_interval.h.in" "${CMAKE_BINARY_DIR}/generated/min_update_interval.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/src/template/mkquery_config.h.in" "${CMAKE_BINARY_DIR}/generated/mkquery_config.h" @ONLY)

# ==============================================================================
# Dependencies (via CPM.cmake)
# ==============================================================================
include(CPM)

find_package(OpenSSL 3.0 REQUIRED)

# ---------------------------------------------------------------------------
# glaze – JSON/reflection library
# ---------------------------------------------------------------------------
CPMAddPackage("gh:stephenberry/glaze@7.8.2")

# ---------------------------------------------------------------------------
# fmt / std::format  (conditional)
# ---------------------------------------------------------------------------
if (YADDNSC_USE_SYSTEM_SPDLOG)
  # System spdlog links against libfmt internally, so we must use the system
  # fmt library too.  std::format (C++23) is deliberately NOT used here because
  # mixing it with libfmt's type system (join_view, etc.) causes silent
  # incompatibilities and build failures.
  if (HAVE_STD_FORMAT)
    message(STATUS "Using system fmt (std::format is available but disabled — "
                   "system spdlog requires libfmt)")
  else ()
    message(STATUS "Using system fmt library")
  endif ()
  find_package(fmt REQUIRED)
elseif (HAVE_STD_FORMAT)
  message(STATUS "Using native std::format")
  set(SPDLOG_USE_STD_FORMAT ON)
  set(YADDNSC_USE_STD_FORMAT ON)
else ()
  message(STATUS "Using external fmt library via CPM")
  CPMAddPackage("gh:fmtlib/fmt#12.2.0")
  set(SPDLOG_FMT_EXTERNAL ON)
endif ()

# ---------------------------------------------------------------------------
# spdlog – logging library
# ---------------------------------------------------------------------------
if (YADDNSC_USE_SYSTEM_SPDLOG)
  find_package(spdlog REQUIRED)
  message(STATUS "Using system spdlog (${spdlog_VERSION})")
else ()
  CPMAddPackage("gh:gabime/spdlog@1.17.0")
  set_target_properties(spdlog PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif ()

# ---------------------------------------------------------------------------
# cpp-httplib – HTTP client/server
# ---------------------------------------------------------------------------
CPMAddPackage(
        URI "gh:yhirose/cpp-httplib@0.48.0"
        OPTIONS "HTTPLIB_REQUIRE_OPENSSL ON"
        "HTTPLIB_USE_ZLIB_IF_AVAILABLE ON"
        "HTTPLIB_USE_BROTLI_IF_AVAILABLE ON"
)

# ---------------------------------------------------------------------------
# CLI11 – command line parser
# ---------------------------------------------------------------------------
CPMAddPackage("gh:CLIUtils/CLI11@2.6.2")

# ---------------------------------------------------------------------------
# magic_enum – static reflection for enums (header-only)
# ---------------------------------------------------------------------------
CPMAddPackage("gh:Neargye/magic_enum@0.9.8")

# ---------------------------------------------------------------------------
# BS::thread_pool – header-only, no CMakeLists.txt
# ---------------------------------------------------------------------------
CPMAddPackage(
        URI "gh:bshoshany/thread-pool@5.1.0"
        DOWNLOAD_ONLY YES
)
if (thread-pool_ADDED)
  add_library(BS_thread_pool INTERFACE)
  target_include_directories(BS_thread_pool INTERFACE ${thread-pool_SOURCE_DIR}/include)
endif ()

# ==============================================================================
# Main executable
# ==============================================================================
add_executable(yaddnsc
        # entry
        src/main.cpp
        # config
        src/config/config.cpp
        # core
        src/core/updater.cpp
        src/core/manager.cpp
        src/core/driver_manager.cpp
        src/core/driver_loader.cpp
        src/core/scheduler.cpp
        src/core/core_logger.cpp
        src/core/signal_handler.cpp
        # network
        src/network/http_client.cpp
        src/network/inet_address.cpp
        src/network/net_devices.cpp
        src/network/uri.cpp
        # cli
        src/cli/cli.cpp
        src/cli/driver.cpp
        src/cli/interface.cpp
        src/cli/dns.cpp
        src/cli/config.cpp
        # dns
        src/dns/error.cpp
        src/dns/proto/parser.cpp
        src/dns/proto/mkquery.cpp
        src/dns/resolver/classic.cpp
        src/dns/resolver/doh.cpp
        src/dns/resolver/dot.cpp
        src/dns/dispatcher.cpp
        src/dns/factory.cpp
        # ip_source
        src/ip_source/iface_util.cpp
        src/ip_source/iface.cpp
        src/ip_source/http.cpp
        src/ip_source/mdns.cpp
        src/ip_source/factory.cpp
)

# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------
target_compile_options(yaddnsc PRIVATE
    -Wall -Wextra
    # Hide all symbols at compile time; only YADDNSC_EXPORT-marked functions
    # have STV_DEFAULT visibility.
    -fvisibility=hidden
    # Clang 19.x needs -fexperimental-library for std::stop_token, std::jthread, etc.
    $<$<AND:$<CXX_COMPILER_ID:Clang>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,19>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,20>>:-fexperimental-library>
    # Debug symbols
    $<$<CONFIG:Debug>:-ggdb -gdwarf-3>
)

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
target_include_directories(yaddnsc PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

if (glaze_ADDED)
  target_include_directories(yaddnsc SYSTEM PRIVATE ${glaze_SOURCE_DIR}/include)
endif ()

# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------
target_compile_definitions(yaddnsc PRIVATE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
    CPPHTTPLIB_OPENSSL_SUPPORT
    $<$<BOOL:${YADDNSC_USE_STD_FORMAT}>:YADDNSC_USE_STD_FORMAT>
)

# ---------------------------------------------------------------------------
# Linker options
# ---------------------------------------------------------------------------
# Strip symbols in Release builds
target_link_options(yaddnsc PRIVATE
    $<$<CONFIG:Release>:$<$<PLATFORM_ID:Darwin>:-Wl,-S>>
    $<$<CONFIG:Release>:$<$<NOT:$<PLATFORM_ID:Darwin>>:-Wl,-s>>
)

# ---------------------------------------------------------------------------
# Target properties
# ---------------------------------------------------------------------------
# LTO is incompatible with musl (auto-detected in FeatureCheck)
if (NOT LIBC_MUSL)
  set_target_properties(yaddnsc PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

# --export-dynamic puts exported symbols into .dynsym so dlsym / dlopen can find them.
# ENABLE_EXPORTS requests this from CMake, but CMake 4.x + AppleClang may not emit
# the flag correctly, so we add -rdynamic explicitly on all non-Windows platforms.
set_target_properties(yaddnsc PROPERTIES ENABLE_EXPORTS ON)
target_link_options(yaddnsc PRIVATE
    $<$<NOT:$<PLATFORM_ID:Windows>>:-rdynamic>
)

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
target_link_libraries(yaddnsc PRIVATE BS_thread_pool magic_enum httplib spdlog::spdlog OpenSSL::SSL OpenSSL::Crypto
        CLI11::CLI11 glaze::glaze ${CMAKE_DL_LIBS}
)

# interface target: wraps fmt so consumers always get the right dep and define regardless of HAVE_STD_FORMAT
add_library(yaddnsc_fmt INTERFACE)
target_include_directories(yaddnsc_fmt INTERFACE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(yaddnsc_fmt INTERFACE
    $<$<BOOL:${YADDNSC_USE_STD_FORMAT}>:YADDNSC_USE_STD_FORMAT>
)
if (NOT HAVE_STD_FORMAT)
  target_link_libraries(yaddnsc_fmt INTERFACE fmt::fmt)
endif ()
target_link_libraries(yaddnsc PRIVATE yaddnsc_fmt)

# resolv is linux-specific, not present on BSDs
find_library(RESOLV resolv)
if (RESOLV)
  target_link_libraries(yaddnsc PRIVATE ${RESOLV})
endif ()

# ==============================================================================
# Build configuration summary
# ==============================================================================
message(STATUS "Minimum update interval: ${YADDNSC_MIN_UPDATE_INTERVAL}s")

if (YADDNSC_USE_SYSTEM_SPDLOG)
  message(STATUS "spdlog: system package")
else ()
  message(STATUS "spdlog: bundled (CPM)")
endif ()

if (YADDNSC_MANUAL_MKQUERY)
  message(STATUS "DNS query builder: manual (no libresolv)")
else ()
  message(STATUS "DNS query builder: system (res_mkquery)")
endif ()

# ==============================================================================
# Output directory
# ==============================================================================
set_target_properties(yaddnsc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/objs"
)

# ==============================================================================
# Driver modules
# ==============================================================================
add_subdirectory(${PROJECT_SOURCE_DIR}/driver/)
