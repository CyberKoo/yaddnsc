cmake_minimum_required(VERSION 3.28...3.31 FATAL_ERROR)
project(yaddnsc)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# ==============================================================================
# Compiler & project-wide settings
# ==============================================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")

# Clang 19.x needs -fexperimental-library for std::stop_token, std::jthread, etc.
if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "19" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "20")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexperimental-library")
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

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb -gdwarf-3 -Wall")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/objs/lib)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/objs)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# default to release if not specified
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif ()

# ==============================================================================
# Feature detection
# ==============================================================================
include(FeatureCheck)

# ==============================================================================
# Release build flags
# ==============================================================================
# LTO is incompatible with musl (auto-detected in FeatureCheck)
if (NOT LIBC_MUSL)
  set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto=auto")
endif ()

# Strip symbols in Release builds
if (APPLE)
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -Wl,-S")
else ()
  set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -Wl,-s")
endif ()

# ==============================================================================
# Version info
# ==============================================================================
set(YADDNSC_VERSION_MAJOR 1)
set(YADDNSC_VERSION_MINOR 0)
set(YADDNSC_VERSION_PATCH 0)
set(YADDNSC_DEVELOPMENT ON)
set(YADDNSC_RELEASE_INFO RELEASE)
set(YADDNSC_DRV_VERSION "1100000")

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
configure_file("${CMAKE_SOURCE_DIR}/src/template/dns_mkquery_config.h.in" "${CMAKE_BINARY_DIR}/generated/dns_mkquery_config.h" @ONLY)

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
if (HAVE_STD_FORMAT)
  message(STATUS "Using native std::format")
  add_compile_definitions(YADDNSC_USE_STD_FORMAT)
  # SPDLOG_USE_STD_FORMAT is only meaningful when building spdlog from source,
  # so skip it when using system spdlog (it was already configured at build time).
  if (NOT YADDNSC_USE_SYSTEM_SPDLOG)
    set(SPDLOG_USE_STD_FORMAT ON)
  endif ()
elseif (YADDNSC_USE_SYSTEM_SPDLOG)
  message(STATUS "Using system fmt library (find_package)")
  find_package(fmt REQUIRED)
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
add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

# ---------------------------------------------------------------------------
# cpp-httplib – HTTP client/server
# ---------------------------------------------------------------------------
CPMAddPackage(
        URI "gh:yhirose/cpp-httplib@0.48.0"
        OPTIONS "HTTPLIB_REQUIRE_OPENSSL ON"
        "HTTPLIB_USE_ZLIB_IF_AVAILABLE ON"
        "HTTPLIB_USE_BROTLI_IF_AVAILABLE ON"
)
add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)

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
# Include paths (inherited by driver subdirectory)
# ==============================================================================
include_directories(${PROJECT_SOURCE_DIR}/include/)
include_directories(${PROJECT_SOURCE_DIR}/src/)
include_directories(${CMAKE_BINARY_DIR}/generated/)

# Glaze include path — propagated to all subdirectories (including drivers)
# so they can consume glaze headers without linking the library.
if (glaze_ADDED)
  include_directories(SYSTEM ${glaze_SOURCE_DIR}/include)
endif ()

# ==============================================================================
# Source files
# ==============================================================================
set(URI src/util/uri.cpp)
set(Config src/config/config.cpp)
set(Updater src/core/updater.cpp)
set(Manager src/core/manager.cpp)
set(CertUtil src/util/cert_util.cpp)
set(Scheduler src/core/scheduler.cpp)
set(CoreLogger src/core/core_logger.cpp)
set(HttpClient src/network/http_client.cpp)
set(DriverLoader src/core/driver_loader.cpp)
set(InetAddress src/network/inet_address.cpp)
set(DriverManager src/core/driver_manager.cpp)
set(SignalHandler src/core/signal_handler.cpp)
set(NetworkManager src/network/network_manager.cpp)
set(DNS src/dns/types.cpp src/dns/classic.cpp src/dns/parser.cpp src/dns/doh.cpp src/dns/dot.cpp src/dns/dns_mkquery.cpp src/dns/multi_resolver.cpp src/dns/resolver_factory.cpp)
set(CLI src/cli/cli.cpp src/cli/driver.cpp src/cli/interface.cpp src/cli/dns.cpp src/cli/config.cpp)

# ==============================================================================
# Main executable
# ==============================================================================
add_executable(yaddnsc src/main.cpp src/util/string_util.cpp ${CertUtil} ${DNS} ${Config} ${DriverManager}
        ${DriverLoader} ${URI} ${Updater} ${Manager} ${SignalHandler} ${Scheduler} ${HttpClient} ${NetworkManager}
        ${CoreLogger} ${InetAddress} ${CLI}
)

# Hide all symbols at compile time; only YADDNSC_EXPORT-marked functions
# have STV_DEFAULT visibility.
# --export-dynamic puts them into .dynsym so dlsym can find them, while
# STV_HIDDEN symbols stay in .dynsym but are invisible to dlsym.
target_compile_options(yaddnsc PRIVATE -fvisibility=hidden)
set_target_properties(yaddnsc PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(yaddnsc PRIVATE BS_thread_pool magic_enum httplib spdlog::spdlog OpenSSL::SSL OpenSSL::Crypto
        CLI11::CLI11 glaze::glaze ${CMAKE_DL_LIBS}
)

# interface target: wraps fmt so consumers always get the right dep regardless of HAVE_STD_FORMAT
add_library(yaddnsc_fmt INTERFACE)
target_include_directories(yaddnsc_fmt INTERFACE ${CMAKE_SOURCE_DIR}/include)
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
# Driver modules
# ==============================================================================
add_subdirectory(${PROJECT_SOURCE_DIR}/driver/)
