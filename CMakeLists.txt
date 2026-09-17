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

# ==============================================================================
# Version info
# ==============================================================================
# Version is now managed by project(yaddnsc VERSION 1.0.0).
# The version.h.in template uses @PROJECT_VERSION_MAJOR@ etc. directly,
# so no YADDNSC_VERSION_* aliases are needed here.

option(YADDNSC_DEVELOPMENT "Development mode (embed git version info)" ON)
include(ReleaseInfo)

# ==============================================================================
# User options (cache variables, overridable via -D)
# ==============================================================================
set(YADDNSC_LOGGING_PATTERN "[%D %T.%e] [%^%8l%$] [%8!t] [%15!s:%-4#] %v" CACHE STRING "Logging pattern passed to spdlog::set_pattern()")
set(YADDNSC_MIN_UPDATE_INTERVAL 60 CACHE STRING "Minimum allowed update interval (seconds)")
set(YADDNSC_DEFAULT_DNS_SERVER "1.1.1.1" CACHE STRING "Default DNS server address when none is configured")
set(YADDNSC_DEFAULT_DNS_PORT 53 CACHE STRING "Default DNS server port when none is configured")

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

# System fmt < 10 (Ubuntu 24.04 ships 9.1.0) hits a GCC 14 false-positive
# -Warray-bounds inside fmt's bigint (dragon float formatting path,
# fmtlib/fmt#3731, fixed in fmt 10). -Werror turns it into a hard error;
# demote the warning for this combination only so real array-bounds bugs
# in project code still surface as warnings.
if (YADDNSC_USE_SYSTEM_SPDLOG AND fmt_VERSION VERSION_LESS 10)
  target_compile_options(yaddnsc_compile_config INTERFACE -Wno-error=array-bounds)
endif ()

target_include_directories(yaddnsc_compile_config INTERFACE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

target_compile_definitions(yaddnsc_compile_config INTERFACE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
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
    BS_thread_pool
    picohttpparser
    yaddnsc_fmt
)

# ==============================================================================
# OBJECT libraries — each module compiles separately, all .o linked into main exe.
# Using OBJECT libraries avoids the static archive dead-stripping problem so
# registration via static initialization registers correctly.
# ==============================================================================

add_library(yaddnsc_config OBJECT
    src/config/config.cpp
    src/config/normalizer.cpp
    src/config/static_validator.cpp
)
target_link_libraries(yaddnsc_config PRIVATE yaddnsc_compile_config)

# Domain layer — pure rules and value types (no I/O, no threads, no clocks).
add_library(yaddnsc_domain OBJECT
    src/domain/update/schedule_queue.cpp
)
target_link_libraries(yaddnsc_domain PRIVATE yaddnsc_compile_config)

# Application layer — use cases and scheduling loop over the ports.
add_library(yaddnsc_application OBJECT
    src/application/update_workflow.cpp
    src/application/scheduler_runner.cpp
    src/application/pool_task_executor.cpp
    src/application/run_lifecycle.cpp
    src/application/diagnostics.cpp
    src/application/environment_validator.cpp
)
target_link_libraries(yaddnsc_application PRIVATE yaddnsc_compile_config)

add_library(yaddnsc_core OBJECT
    src/core/driver_loader.cpp
    src/core/signal_watcher.cpp
    src/core/spdlog_logger.cpp
    src/core/steady_clock.cpp
)
target_link_libraries(yaddnsc_core PRIVATE yaddnsc_compile_config)

# Plugin host infrastructure — the v1 alpha C ABI loader, catalog, Host
# Services and driver gateway.
add_library(yaddnsc_plugin_host OBJECT
    src/infrastructure/plugin/shared_library.cpp
    src/infrastructure/plugin/plugin_loader.cpp
    src/infrastructure/plugin/driver_catalog.cpp
    src/infrastructure/plugin/host_services.cpp
    src/infrastructure/plugin/abi_driver_gateway.cpp
)
target_link_libraries(yaddnsc_plugin_host PRIVATE yaddnsc_compile_config)

add_library(yaddnsc_network OBJECT
    src/network/inet_address.cpp
    src/network/net_devices.cpp
    src/network/socket.cpp
    src/network/socket_addr.cpp
    src/network/system_network_interfaces.cpp
    src/network/uri.cpp
)
target_link_libraries(yaddnsc_network PRIVATE yaddnsc_compile_config)

# Transport layer (net::transport) and the HTTP client domain (net::http).
add_library(yaddnsc_net_transport OBJECT
    src/network/transport/detail/socket_stream.cpp
    src/network/transport/tls_stream.cpp
    src/network/transport/tcp_stream.cpp
)
target_link_libraries(yaddnsc_net_transport PRIVATE yaddnsc_compile_config)

# HTTP client domain (net::http) — linked into the main executable and used
# by drivers (via HttpClient), the HTTP IP source (PersistentClient) and DoH.
add_library(yaddnsc_http_client OBJECT
    src/http_client/protocol/exchange.cpp
    src/http_client/redirect.cpp
    src/http_client/stream_factory.cpp
    src/http_client/wire_request.cpp
    src/http_client/session.cpp
    src/http_client/client.cpp
    src/http_client/persistent_client.cpp
)
target_link_libraries(yaddnsc_http_client PRIVATE yaddnsc_compile_config)
target_link_libraries(yaddnsc_http_client PRIVATE picohttpparser)

add_library(yaddnsc_cli OBJECT
    src/cli/parser.cpp
    src/cli/presenter.cpp
)
target_link_libraries(yaddnsc_cli PRIVATE yaddnsc_compile_config)

# Composition root — assembles concrete dependencies and dispatches commands.
add_library(yaddnsc_composition OBJECT
    src/composition/bootstrap.cpp
)
target_link_libraries(yaddnsc_composition PRIVATE yaddnsc_compile_config)

add_library(yaddnsc_dns OBJECT
    src/dns/error.cpp
    src/dns/validator.cpp
    src/dns/wire/builder.cpp
    src/dns/resolver/doh.cpp
    src/dns/resolver/dot.cpp
    src/dns/factory.cpp
    src/dns/resolver_catalog.cpp
)
target_link_libraries(yaddnsc_dns PRIVATE yaddnsc_compile_config)

add_library(yaddnsc_ip_source OBJECT
    src/ip_source/iface_util.cpp
    src/ip_source/iface.cpp
    src/ip_source/http.cpp
    src/ip_source/mdns.cpp
    src/ip_source/mdns_response.cpp
    src/ip_source/factory.cpp
    src/ip_source/adapter.cpp
)
target_link_libraries(yaddnsc_ip_source PRIVATE yaddnsc_compile_config)

add_library(yaddnsc_util OBJECT
    src/util/cert_util.cpp
)
target_link_libraries(yaddnsc_util PRIVATE yaddnsc_compile_config)
target_link_libraries(yaddnsc_util PUBLIC OpenSSL::Crypto)

target_sources(yaddnsc_dns PRIVATE
    src/dns/resolver/classic.cpp
    src/dns/parser.cpp
    src/dns/dispatcher.cpp
)

# Sanitizer compile flags on all OBJECT libraries (link flags are a no-op on OBJECT libs
# and only matter on the final executable, where add_sanitizer_flags is called again).
foreach(tgt yaddnsc_config yaddnsc_domain yaddnsc_application yaddnsc_core yaddnsc_plugin_host yaddnsc_network yaddnsc_net_transport yaddnsc_http_client yaddnsc_cli yaddnsc_composition yaddnsc_dns yaddnsc_ip_source yaddnsc_util)
  add_sanitizer_flags(${tgt})
endforeach()

# ==============================================================================
# Main executable — only main.cpp + OBJECT library .o files
# ==============================================================================
add_executable(yaddnsc
    src/main.cpp
    $<TARGET_OBJECTS:yaddnsc_config>
    $<TARGET_OBJECTS:yaddnsc_domain>
    $<TARGET_OBJECTS:yaddnsc_application>
    $<TARGET_OBJECTS:yaddnsc_core>
    $<TARGET_OBJECTS:yaddnsc_plugin_host>
    $<TARGET_OBJECTS:yaddnsc_network>
    $<TARGET_OBJECTS:yaddnsc_net_transport>
    $<TARGET_OBJECTS:yaddnsc_http_client>
    $<TARGET_OBJECTS:yaddnsc_cli>
    $<TARGET_OBJECTS:yaddnsc_composition>
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
endif ()

# --export-dynamic is intentionally NOT set: driver plugins load through the
# C ABI alone and must not resolve any host symbols.

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
target_link_libraries(yaddnsc PRIVATE BS_thread_pool magic_enum spdlog::spdlog OpenSSL::SSL OpenSSL::Crypto
        CLI11::CLI11 glaze::glaze picohttpparser ${CMAKE_DL_LIBS}
)

# ==============================================================================
# Build configuration summary
# ==============================================================================
message(STATUS "Minimum update interval: ${YADDNSC_MIN_UPDATE_INTERVAL}s")

if (YADDNSC_USE_SYSTEM_SPDLOG)
  message(STATUS "spdlog: system package")
else ()
  message(STATUS "spdlog: bundled (CPM)")
endif ()

message(STATUS "DNS stack: built-in (no libresolv)")

message(STATUS "Default DNS server: ${YADDNSC_DEFAULT_DNS_SERVER}:${YADDNSC_DEFAULT_DNS_PORT}")

# ==============================================================================
# Documentation — Doxygen
# ==============================================================================
include(Doxygen)

# ==============================================================================
# Driver modules
# ==============================================================================
# SDK interface for driver plugins — the ONLY supported surface: the v1 alpha
# C ABI plus the C++ helper layer (yaddnsc/sdk/*). Deliberately does NOT expose
# the host's src/ include root or host-only dependencies.
add_library(yaddnsc_sdk INTERFACE)
target_include_directories(yaddnsc_sdk INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(yaddnsc_sdk INTERFACE glaze::glaze)
target_compile_options(yaddnsc_sdk INTERFACE -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)

add_subdirectory(${PROJECT_SOURCE_DIR}/plugin_crypto/)
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
