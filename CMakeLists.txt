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

include(ClangTidy)

# ==============================================================================
# Build type configuration
# ==============================================================================

# Default to Debug for developer experience (debug symbols, sanitizers).
# Use -DCMAKE_BUILD_TYPE=Release for optimized production builds.
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif ()

# ==============================================================================
# Compiler version requirements
# ==============================================================================
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "14")
    message(FATAL_ERROR "GCC ${CMAKE_CXX_COMPILER_VERSION} is too old. yaddnsc requires GCC 14+ (C++23).")
  endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19")
    message(FATAL_ERROR "Clang ${CMAKE_CXX_COMPILER_VERSION} is too old. yaddnsc requires Clang 19+ (C++23 + libc++ experimental features).")
  endif ()
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS "15")
    message(FATAL_ERROR "AppleClang ${CMAKE_CXX_COMPILER_VERSION} is too old. yaddnsc requires AppleClang 15+ (C++23).")
  endif ()
else ()
  message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. yaddnsc requires GCC 14+, Clang 19+, or AppleClang 15+.")
endif ()

# ==============================================================================
# Feature detection
# ==============================================================================
include(FeatureCheck)

# Propagate to the CMake variable expected by config_cmake.h.in (#cmakedefine YADDNSC_LIBC_MUSL)
set(YADDNSC_LIBC_MUSL ${LIBC_MUSL})

include(Sanitizer)

# ==============================================================================
# Release build flags
# ==============================================================================
# Native DNS is the default on all platforms. The built-in stack provides
# better portability and full control over the transport layer.
# Set YADDNSC_USE_NATIVE_DNS=OFF to fall back to system libresolv.

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

option(YADDNSC_DEVELOPMENT "Development mode (embed git version info)" ON)
include(ReleaseInfo)

# ==============================================================================
# User options (cache variables, overridable via -D)
# ==============================================================================
set(YADDNSC_LOGGING_PATTERN "[%D %T.%e] [%^%8l%$] [%8!t] [%15!s:%-4#] %v" CACHE STRING "Logging pattern passed to spdlog::set_pattern()")
set(YADDNSC_MIN_UPDATE_INTERVAL 60 CACHE STRING "Minimum allowed update interval (seconds)")
set(YADDNSC_DEFAULT_DNS_SERVER "1.1.1.1" CACHE STRING "Default DNS server address when none is configured")
set(YADDNSC_DEFAULT_DNS_PORT 53 CACHE STRING "Default DNS server port when none is configured")

option(YADDNSC_USE_NATIVE_DNS "Use built-in DNS stack instead of system libresolv" ON)
option(YADDNSC_USE_SYSTEM_SPDLOG "Use spdlog from the system instead of the bundled CPM version" OFF)

# Debug-mode sanitizers can be disabled by downstream CI jobs (e.g., coverage)
# that need to combine Debug with incompatible flags like --coverage.
option(YADDNSC_SANITIZE_DEBUG "Enable sanitizers in Debug builds" ON)

if (YADDNSC_MIN_UPDATE_INTERVAL LESS 0)
  message(FATAL_ERROR "YADDNSC_MIN_UPDATE_INTERVAL must not be negative, got ${YADDNSC_MIN_UPDATE_INTERVAL}")
endif ()

# ---------------------------------------------------------------------------
# Validate YADDNSC_DEFAULT_DNS_SERVER — must be a valid IPv4 or IPv6 address
# ---------------------------------------------------------------------------
include(ValidateIpAddress)
validate_ip_address("${YADDNSC_DEFAULT_DNS_SERVER}" "YADDNSC_DEFAULT_DNS_SERVER")

# ==============================================================================
# Build ID — compiler identity fields used by build_id.hpp
# ==============================================================================
set(YADDNSC_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(YADDNSC_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(YADDNSC_BUILD_ID_STR "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} ${CMAKE_BUILD_TYPE}")

# ==============================================================================
include(GNUInstallDirs)

# Generated headers (all configure_file calls, after all variables are defined)
# ==============================================================================
configure_file("${CMAKE_SOURCE_DIR}/template/headers/version.h.in" "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/template/headers/build_id.hpp.in" "${CMAKE_BINARY_DIR}/generated/build_id.hpp" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/template/headers/driver_ver.h.in" "${CMAKE_BINARY_DIR}/generated/driver_ver.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/template/headers/config_cmake.h.in" "${CMAKE_BINARY_DIR}/generated/config_cmake.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/template/headers/logging_pattern.h.in" "${CMAKE_BINARY_DIR}/generated/logging_pattern.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/template/headers/min_update_interval.h.in" "${CMAKE_BINARY_DIR}/generated/min_update_interval.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/template/headers/resolver_config.h.in" "${CMAKE_BINARY_DIR}/generated/resolver_config.h" @ONLY)

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
# picohttpparser – HTTP request/response parser (C library)
# ---------------------------------------------------------------------------
CPMAddPackage("gh:h2o/picohttpparser@1.2")
if (picohttpparser_ADDED)
  add_library(picohttpparser STATIC ${picohttpparser_SOURCE_DIR}/picohttpparser.c)
  target_include_directories(picohttpparser PUBLIC ${picohttpparser_SOURCE_DIR})
endif ()

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
# Common compile configuration (shared across all OBJECT libs + main exe)
# ==============================================================================

# interface target: wraps fmt so consumers always get the right dep and define
add_library(yaddnsc_fmt INTERFACE)
target_include_directories(yaddnsc_fmt INTERFACE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(yaddnsc_fmt INTERFACE
        $<$<BOOL:${YADDNSC_USE_STD_FORMAT}>:YADDNSC_USE_STD_FORMAT>
)
if (NOT HAVE_STD_FORMAT)
  target_link_libraries(yaddnsc_fmt INTERFACE fmt::fmt)
endif ()

add_library(yaddnsc_compile_config INTERFACE)

target_compile_options(yaddnsc_compile_config INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
    -fvisibility=hidden
    $<$<AND:$<CXX_COMPILER_ID:Clang>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,19>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,20>>:-fexperimental-library>
    $<$<CONFIG:Debug>:-ggdb>
    $<$<CONFIG:Debug>:-gdwarf-3>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
    # GCC 14 with -O1 (Sanitizer build type) produces false-positive
    # -Wmaybe-uninitialized warnings in <regex> internals (std::function
    # move constructor in std::__detail::_State). -Wall enables this
    # warning, and -Werror promotes it to a hard error.  Suppress it
    # for Sanitizer builds where -O1 is required for instrumentation.
    $<$<CONFIG:Sanitizer>:-Wno-maybe-uninitialized>
)

target_include_directories(yaddnsc_compile_config INTERFACE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

target_compile_definitions(yaddnsc_compile_config INTERFACE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
    CPPHTTPLIB_OPENSSL_SUPPORT
    $<$<BOOL:${YADDNSC_USE_STD_FORMAT}>:YADDNSC_USE_STD_FORMAT>
    $<$<BOOL:${YADDNSC_USE_SYSTEM_SPDLOG}>:YADDNSC_USE_SYSTEM_SPDLOG>
)

# Common third-party dependencies for include path propagation.
# OBJECT libraries need these to resolve #include directives at compile time.
# The actual link step still happens on the executable via its own
# target_link_libraries call below.
target_link_libraries(yaddnsc_compile_config INTERFACE
    spdlog::spdlog
    glaze::glaze
    magic_enum
    OpenSSL::SSL
    OpenSSL::Crypto
    CLI11::CLI11
    httplib
    BS_thread_pool
    picohttpparser
    yaddnsc_fmt
)

# ==============================================================================
# OBJECT libraries — each module compiles separately, all .o linked into main exe.
# Using OBJECT libraries avoids the static archive dead-stripping problem:
#   - registration via static initialization registers correctly
#   - -rdynamic exports work naturally
# ==============================================================================

add_library(yaddnsc_config OBJECT
    src/config/config.cpp
)
target_link_libraries(yaddnsc_config PRIVATE yaddnsc_compile_config)

add_library(yaddnsc_core OBJECT
    src/core/updater.cpp
    src/core/manager.cpp
    src/core/driver_manager.cpp
    src/core/driver_loader.cpp
    src/core/scheduler.cpp
    src/core/core_logger.cpp
    src/core/signal_watcher.cpp
)
target_link_libraries(yaddnsc_core PRIVATE yaddnsc_compile_config)

add_library(yaddnsc_http OBJECT
    src/http/header_parser.cpp
    src/http/body_parser.cpp
    src/http/request.cpp
    src/http/http.cpp
)
target_link_libraries(yaddnsc_http PRIVATE yaddnsc_compile_config)
target_link_libraries(yaddnsc_http PRIVATE picohttpparser magic_enum)

add_library(yaddnsc_network OBJECT
    src/network/http_client.cpp
    src/network/inet_address.cpp
    src/network/net_devices.cpp
    src/network/socket.cpp
    src/network/socket_addr.cpp
    src/network/uri.cpp
    src/network/tls_connection.cpp
    src/network/transport/tls_stream.cpp
)
target_link_libraries(yaddnsc_network PRIVATE yaddnsc_compile_config)

add_library(yaddnsc_cli OBJECT
    src/cli/cli.cpp
    src/cli/driver.cpp
    src/cli/interface.cpp
    src/cli/dns.cpp
    src/cli/config.cpp
    src/cli/info.cpp
)
target_link_libraries(yaddnsc_cli PRIVATE yaddnsc_compile_config)

add_library(yaddnsc_dns OBJECT
    src/dns/error.cpp
    src/dns/validator.cpp
    src/dns/wire/builder.cpp
    src/dns/resolver/doh.cpp
    src/dns/resolver/dot.cpp
    src/dns/factory.cpp
    src/dns/resolver_registry.cpp
)
target_link_libraries(yaddnsc_dns PRIVATE yaddnsc_compile_config)

add_library(yaddnsc_ip_source OBJECT
    src/ip_source/iface_util.cpp
    src/ip_source/iface.cpp
    src/ip_source/http.cpp
    src/ip_source/mdns.cpp
    src/ip_source/factory.cpp
)
target_link_libraries(yaddnsc_ip_source PRIVATE yaddnsc_compile_config)

add_library(yaddnsc_util OBJECT
    src/util/signing.cpp
    src/util/cert_util.cpp
)
target_link_libraries(yaddnsc_util PRIVATE yaddnsc_compile_config)
target_link_libraries(yaddnsc_util PUBLIC OpenSSL::Crypto)

# Compile only the selected resolver implementation.
if (YADDNSC_USE_NATIVE_DNS)
  target_sources(yaddnsc_dns PRIVATE
      src/dns/resolver/classic_native.cpp
      src/dns/parser/parser_native.cpp
      src/dns/dispatcher.cpp
  )
else ()
  target_sources(yaddnsc_dns PRIVATE
      src/dns/resolver/classic_system.cpp
      src/dns/parser/parser_system.cpp
      src/dns/dispatcher_system.cpp
  )
endif ()

# Sanitizer compile flags on all OBJECT libraries (link flags are a no-op on OBJECT libs
# and only matter on the final executable, where add_sanitizer_flags is called again).
foreach(tgt yaddnsc_config yaddnsc_core yaddnsc_http yaddnsc_network yaddnsc_cli yaddnsc_dns yaddnsc_ip_source yaddnsc_util)
  add_sanitizer_flags(${tgt})
endforeach()

# ==============================================================================
# Main executable — only main.cpp + OBJECT library .o files
# ==============================================================================
add_executable(yaddnsc
    src/main.cpp
    $<TARGET_OBJECTS:yaddnsc_config>
    $<TARGET_OBJECTS:yaddnsc_core>
    $<TARGET_OBJECTS:yaddnsc_http>
    $<TARGET_OBJECTS:yaddnsc_network>
    $<TARGET_OBJECTS:yaddnsc_cli>
    $<TARGET_OBJECTS:yaddnsc_dns>
    $<TARGET_OBJECTS:yaddnsc_ip_source>
    $<TARGET_OBJECTS:yaddnsc_util>
)

target_link_libraries(yaddnsc PRIVATE yaddnsc_compile_config)

# Sanitizer flags for main.cpp and link flags
add_sanitizer_flags(yaddnsc)

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
# LTO is incompatible with musl (auto-detected in FeatureCheck).
# LTO is limited to Release builds only:
#   - Debug (-O0):  no meaningful optimization, adds link-time overhead.
#   - Sanitizer (-O1): small optimization gain, but can interfere with
#     ASan backtraces and UBSan diagnostics.
if (NOT LIBC_MUSL)
  set_target_properties(yaddnsc PROPERTIES INTERPROCEDURAL_OPTIMIZATION
      $<$<CONFIG:Release>:TRUE>)
  # Suppress LTO ODR false positive between DNS::Rcode (dns/types.h)
  # and __ns_rtype (<arpa/nameser.h>) — the two enums share enumerator
  # names (NOERROR, SERVFAIL, NXDOMAIN …) with identical values, causing
  # GCC LTO to incorrectly flag them as an ODR violation.
  target_link_options(yaddnsc PRIVATE -Wno-odr)
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
        CLI11::CLI11 glaze::glaze picohttpparser ${CMAKE_DL_LIBS}
)

# Required only by the system DNS parser (parser_system.cpp, which uses
# ns_initparse / ns_parserr). That translation unit is compiled only when
# YADDNSC_USE_NATIVE_DNS is OFF, so the link guard matches the source guard.
# On musl (where YADDNSC_USE_NATIVE_DNS defaults to ON) libresolv does not
# exist; find_library returns empty and the link is safely skipped.
if (NOT YADDNSC_USE_NATIVE_DNS)
  find_library(RESOLV resolv)
  if (RESOLV)
    target_link_libraries(yaddnsc PRIVATE ${RESOLV})
  endif ()
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

if (YADDNSC_USE_NATIVE_DNS)
  message(STATUS "DNS stack: built-in (no libresolv)")
else ()
  message(DEPRECATION "YADDNSC_USE_NATIVE_DNS is OFF — system libresolv backend is DEPRECATED and will be removed before 1.0.0")
  message(STATUS "DNS stack: system (libresolv) [DEPRECATED]")
endif ()

message(STATUS "Default DNS server: ${YADDNSC_DEFAULT_DNS_SERVER}:${YADDNSC_DEFAULT_DNS_PORT}")

# ==============================================================================
# Documentation — Doxygen
# ==============================================================================
include(Doxygen)

# ==============================================================================
# Driver modules
# ==============================================================================
add_subdirectory(${PROJECT_SOURCE_DIR}/driver/)

# ==============================================================================
# Tests — GoogleTest via CPM, gated by option
# ==============================================================================
option(YADDNSC_BUILD_TESTS "Build unit and integration tests" OFF)
mark_as_advanced(YADDNSC_BUILD_TESTS)

if (YADDNSC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif ()

# ==============================================================================
# Install rules & packaging
# ==============================================================================
include(InstallRules)
