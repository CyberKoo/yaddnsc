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

# Debug-mode sanitizers can be disabled by downstream CI jobs (e.g., coverage)
# that need to combine Debug with incompatible flags like --coverage.
# Defined before Sanitizer.cmake, which evaluates it at include time.
option(YADDNSC_SANITIZE_DEBUG "Enable sanitizers in Debug builds" ON)

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
# Shared build configuration — three focused concerns
# ==============================================================================

# Interface target: wraps fmt so consumers always get the right dep and define.
# The facade header itself is host-internal under src/.
add_library(yaddnsc_fmt INTERFACE)
target_compile_definitions(yaddnsc_fmt INTERFACE
        $<$<BOOL:${YADDNSC_USE_STD_FORMAT}>:YADDNSC_USE_STD_FORMAT>
)
if (NOT HAVE_STD_FORMAT)
  target_link_libraries(yaddnsc_fmt INTERFACE fmt::fmt)
endif ()

# ---------------------------------------------------------------------------
# yaddnsc_warnings — strict warnings for production and test targets
# ---------------------------------------------------------------------------
add_library(yaddnsc_warnings INTERFACE)
target_compile_options(yaddnsc_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Werror
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
  target_compile_options(yaddnsc_warnings INTERFACE -Wno-error=array-bounds)
endif ()

# ---------------------------------------------------------------------------
# yaddnsc_build_options — language level, visibility, project-wide compile
# definitions and include roots. The only public headers are the plugin SDK
# under include/yaddnsc/; host headers live under src/.
# ---------------------------------------------------------------------------
add_library(yaddnsc_build_options INTERFACE)
target_compile_options(yaddnsc_build_options INTERFACE
    -fvisibility=hidden
    $<$<AND:$<CXX_COMPILER_ID:Clang>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,19>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,20>>:-fexperimental-library>
    $<$<CONFIG:Debug>:-ggdb>
    $<$<CONFIG:Debug>:-gdwarf-3>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
)
target_compile_definitions(yaddnsc_build_options INTERFACE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
    $<$<BOOL:${YADDNSC_USE_STD_FORMAT}>:YADDNSC_USE_STD_FORMAT>
    $<$<BOOL:${YADDNSC_USE_SYSTEM_SPDLOG}>:YADDNSC_USE_SYSTEM_SPDLOG>
)
target_include_directories(yaddnsc_build_options INTERFACE
    ${PROJECT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated
)

# ---------------------------------------------------------------------------
# yaddnsc_sanitizers — per-build-type sanitizer flags (cmake/Sanitizer.cmake),
# linked via add_sanitizer_flags(<target>).
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# yaddnsc_internal_headers — the host-internal src/ include root.
# Module headers reference each other through src/-rooted paths
# (e.g. "domain/network/inet_address.h"). PRIVATE to production targets; never
# exposed to driver plugins.
# ---------------------------------------------------------------------------
add_library(yaddnsc_internal_headers INTERFACE)
target_include_directories(yaddnsc_internal_headers INTERFACE ${PROJECT_SOURCE_DIR}/src)

# Baseline shared by every production module target.
function(yaddnsc_production_module TARGET)
  target_link_libraries(${TARGET} PRIVATE
      yaddnsc_warnings
      yaddnsc_build_options
      yaddnsc_internal_headers
  )
  add_sanitizer_flags(${TARGET})
endfunction()

# ==============================================================================
# Production module targets — static libraries with an explicit dependency
# direction:
#
#   domain ← application ← infrastructure/adapters ← composition ← executable
#
# Inter-module links are PUBLIC so tests can link a single module and get
# its transitive closure; third-party dependencies are PRIVATE to the
# modules that actually use them. No static-initialization registration
# exists anywhere in the tree, so plain static archives link correctly.
# ==============================================================================

# TLS support infrastructure (CA certificate discovery).
add_library(yaddnsc_tls_support STATIC
    src/infrastructure/network/tls/cert_util.cpp
)
yaddnsc_production_module(yaddnsc_tls_support)
target_link_libraries(yaddnsc_tls_support PRIVATE OpenSSL::Crypto spdlog::spdlog)

# Domain layer — pure rules and value types (no I/O, threads, or clocks).
# Also owns dns/error.cpp: DnsError is the port-level shared error vocabulary
# (src/domain/error/dns_error.h), so its stringification must be linkable by the
# application layer without pulling in the DNS infrastructure stack.
add_library(yaddnsc_domain STATIC
    src/domain/update/schedule_queue.cpp
    src/domain/network/inet_address.cpp
    src/domain/error/dns_error.cpp
)
yaddnsc_production_module(yaddnsc_domain)
# Domain has no third-party or infrastructure dependency.

# Network infrastructure: OS socket wrappers, devices, and URI handling.
add_library(yaddnsc_network_infrastructure STATIC
    src/infrastructure/network/net_devices.cpp
    src/infrastructure/network/socket.cpp
    src/infrastructure/network/socket_addr.cpp
    src/infrastructure/network/uri.cpp
)
yaddnsc_production_module(yaddnsc_network_infrastructure)
target_link_libraries(yaddnsc_network_infrastructure
    PUBLIC yaddnsc_domain
    PRIVATE spdlog::spdlog yaddnsc_fmt
)

# Network transport infrastructure (net::transport): TCP/TLS streams over OpenSSL.
# OpenSSL is PUBLIC: the public tls_stream.h includes <openssl/ssl.h>, so every
# consumer needs the OpenSSL include path (Homebrew OpenSSL on macOS lives
# outside the default search path).
add_library(yaddnsc_network_transport STATIC
    src/infrastructure/network/transport/detail/socket_stream.cpp
    src/infrastructure/network/transport/tls_stream.cpp
    src/infrastructure/network/transport/tcp_stream.cpp
)
yaddnsc_production_module(yaddnsc_network_transport)
target_link_libraries(yaddnsc_network_transport
    PUBLIC yaddnsc_network_infrastructure yaddnsc_tls_support OpenSSL::SSL OpenSSL::Crypto
    PRIVATE spdlog::spdlog yaddnsc_fmt
)

# HTTP client infrastructure (net::http) — used by the HTTP IP source
# (PersistentClient), DoH and the driver Host Services.
add_library(yaddnsc_http_infrastructure STATIC
    src/infrastructure/network/http/protocol/exchange.cpp
    src/infrastructure/network/http/redirect.cpp
    src/infrastructure/network/http/stream_factory.cpp
    src/infrastructure/network/http/wire_request.cpp
    src/infrastructure/network/http/session.cpp
    src/infrastructure/network/http/client.cpp
    src/infrastructure/network/http/persistent_client.cpp
)
yaddnsc_production_module(yaddnsc_http_infrastructure)
target_link_libraries(yaddnsc_http_infrastructure
    PUBLIC yaddnsc_network_transport
    PRIVATE picohttpparser spdlog::spdlog yaddnsc_fmt
)

# Application layer — use cases and the scheduling loop over the ports.
# Deliberately free of CLI11, OpenSSL, Glaze, spdlog and dlopen: logging
# goes through the ports/log.h facade.
add_library(yaddnsc_application STATIC
    src/application/update_workflow.cpp
    src/application/scheduler_runner.cpp
    src/application/pool_task_executor.cpp
    src/application/run_lifecycle.cpp
    src/application/diagnostics.cpp
    src/application/environment_validator.cpp
)
yaddnsc_production_module(yaddnsc_application)
target_link_libraries(yaddnsc_application
    PUBLIC yaddnsc_domain yaddnsc_fmt BS_thread_pool
    PRIVATE magic_enum
)

# Configuration infrastructure — JSON parsing, normalization, validation.
add_library(yaddnsc_config_infrastructure STATIC
    src/infrastructure/config/config.cpp
    src/infrastructure/config/normalizer.cpp
    src/infrastructure/config/static_validator.cpp
)
yaddnsc_production_module(yaddnsc_config_infrastructure)
# config/config.h exposes glaze types in its interface.
target_link_libraries(yaddnsc_config_infrastructure
    PUBLIC yaddnsc_domain yaddnsc_network_infrastructure glaze::glaze yaddnsc_fmt
    PRIVATE spdlog::spdlog
)

# DNS infrastructure — wire format, parsers, classic/DoT/DoH resolvers.
# (dns/error.cpp lives in yaddnsc_domain: DnsError is port-level vocabulary.)
add_library(yaddnsc_dns_infrastructure STATIC
    src/infrastructure/dns/validator.cpp
    src/infrastructure/dns/wire/builder.cpp
    src/infrastructure/dns/resolver/doh.cpp
    src/infrastructure/dns/resolver/dot.cpp
    src/infrastructure/dns/resolver/classic.cpp
    src/infrastructure/dns/factory.cpp
    src/infrastructure/dns/resolver_catalog.cpp
    src/infrastructure/dns/parser.cpp
    src/infrastructure/dns/dispatcher.cpp
)
yaddnsc_production_module(yaddnsc_dns_infrastructure)
target_link_libraries(yaddnsc_dns_infrastructure
    PUBLIC yaddnsc_domain yaddnsc_network_infrastructure yaddnsc_http_infrastructure
    PRIVATE spdlog::spdlog magic_enum OpenSSL::Crypto yaddnsc_fmt
)

# IP-source infrastructure — interface / HTTP / mDNS sources + factory.
add_library(yaddnsc_ip_source_infrastructure STATIC
    src/infrastructure/ip_source/iface_util.cpp
    src/infrastructure/ip_source/iface.cpp
    src/infrastructure/ip_source/http.cpp
    src/infrastructure/ip_source/mdns.cpp
    src/infrastructure/ip_source/mdns_response.cpp
    src/infrastructure/ip_source/factory.cpp
    src/infrastructure/ip_source/adapter.cpp
)
yaddnsc_production_module(yaddnsc_ip_source_infrastructure)
target_link_libraries(yaddnsc_ip_source_infrastructure
    PUBLIC yaddnsc_network_infrastructure yaddnsc_dns_infrastructure
    PRIVATE spdlog::spdlog yaddnsc_fmt
)

# Plugin host infrastructure — the v1 alpha C ABI loader, catalog, Host
# Services and driver gateway.
add_library(yaddnsc_plugin_infrastructure STATIC
    src/infrastructure/plugin/shared_library.cpp
    src/infrastructure/plugin/plugin_loader.cpp
    src/infrastructure/plugin/driver_catalog.cpp
    src/infrastructure/plugin/host_services.cpp
    src/infrastructure/plugin/abi_driver_gateway.cpp
)
yaddnsc_production_module(yaddnsc_plugin_infrastructure)
target_link_libraries(yaddnsc_plugin_infrastructure
    PUBLIC yaddnsc_application
    PRIVATE spdlog::spdlog yaddnsc_fmt ${CMAKE_DL_LIBS}
)

# Concrete adapters stay separate so their target dependencies express their
# actual ports and infrastructure requirements.
add_library(yaddnsc_plugin_loader_adapter STATIC
    src/infrastructure/plugin/driver_loader.cpp
)
yaddnsc_production_module(yaddnsc_plugin_loader_adapter)
target_link_libraries(yaddnsc_plugin_loader_adapter
    PUBLIC yaddnsc_plugin_infrastructure
    PRIVATE spdlog::spdlog yaddnsc_fmt
)

add_library(yaddnsc_process_adapter STATIC
    src/infrastructure/process/signal_watcher.cpp
)
yaddnsc_production_module(yaddnsc_process_adapter)
target_link_libraries(yaddnsc_process_adapter
    PRIVATE spdlog::spdlog
)

add_library(yaddnsc_logging_adapter STATIC
    src/infrastructure/logging/spdlog_logger.cpp
)
yaddnsc_production_module(yaddnsc_logging_adapter)
target_link_libraries(yaddnsc_logging_adapter
    PUBLIC yaddnsc_application
    PRIVATE spdlog::spdlog
)

add_library(yaddnsc_time_adapter STATIC
    src/infrastructure/time/steady_clock.cpp
)
yaddnsc_production_module(yaddnsc_time_adapter)
target_link_libraries(yaddnsc_time_adapter PUBLIC yaddnsc_application)

add_library(yaddnsc_network_adapter STATIC
    src/infrastructure/network/system_network_interfaces.cpp
)
yaddnsc_production_module(yaddnsc_network_adapter)
target_link_libraries(yaddnsc_network_adapter
    PUBLIC yaddnsc_application yaddnsc_ip_source_infrastructure
)

# CLI adapter — argument parsing and output presentation.
add_library(yaddnsc_cli_adapter STATIC
    src/cli/parser.cpp
    src/cli/presenter.cpp
)
yaddnsc_production_module(yaddnsc_cli_adapter)
target_link_libraries(yaddnsc_cli_adapter
    PUBLIC yaddnsc_application
    PRIVATE CLI11::CLI11 magic_enum yaddnsc_fmt
)

# Composition root — assembles concrete dependencies and dispatches commands.
add_library(yaddnsc_composition STATIC
    src/composition/bootstrap.cpp
)
yaddnsc_production_module(yaddnsc_composition)
target_link_libraries(yaddnsc_composition
    PUBLIC
        yaddnsc_domain
        yaddnsc_application
        yaddnsc_config_infrastructure
        yaddnsc_dns_infrastructure
        yaddnsc_ip_source_infrastructure
        yaddnsc_plugin_infrastructure
        yaddnsc_plugin_loader_adapter
        yaddnsc_process_adapter
        yaddnsc_logging_adapter
        yaddnsc_time_adapter
        yaddnsc_network_adapter
        yaddnsc_network_infrastructure
        yaddnsc_network_transport
        yaddnsc_http_infrastructure
        yaddnsc_tls_support
        yaddnsc_cli_adapter
    PRIVATE spdlog::spdlog magic_enum yaddnsc_fmt
)

# ==============================================================================
# Main executable — main.cpp + the composition root (which pulls the whole
# module graph transitively)
# ==============================================================================
add_executable(yaddnsc src/main.cpp)
target_link_libraries(yaddnsc PRIVATE
    yaddnsc_warnings
    yaddnsc_build_options
    yaddnsc_internal_headers
    yaddnsc_composition
    yaddnsc_cli_adapter
    spdlog::spdlog
)

# Sanitizer flags for main.cpp and the link step
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

# Third-party link dependencies flow transitively from the module targets
# (composition pulls the full module graph); nothing extra is linked here.

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
add_library(yaddnsc_plugin_sdk INTERFACE)
target_include_directories(yaddnsc_plugin_sdk INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(yaddnsc_plugin_sdk INTERFACE glaze::glaze)
target_compile_options(yaddnsc_plugin_sdk INTERFACE -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden)

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
