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
if(YADDNSC_MIN_UPDATE_INTERVAL LESS 0)
  message(FATAL_ERROR "YADDNSC_MIN_UPDATE_INTERVAL must not be negative, got ${YADDNSC_MIN_UPDATE_INTERVAL}")
endif()

# ==============================================================================
# Generated headers (all configure_file calls, after all variables are defined)
# ==============================================================================
configure_file("${CMAKE_SOURCE_DIR}/include/version.h.in" "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/include/driver_ver.h.in" "${CMAKE_BINARY_DIR}/generated/driver_ver.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/src/template/config_cmake.h.in" "${CMAKE_BINARY_DIR}/generated/config_cmake.h")
configure_file("${CMAKE_SOURCE_DIR}/src/template/logging_pattern.h.in" "${CMAKE_BINARY_DIR}/generated/logging_pattern.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/src/template/min_update_interval.h.in" "${CMAKE_BINARY_DIR}/generated/min_update_interval.h" @ONLY)

# ==============================================================================
# Dependencies (via CPM.cmake)
# ==============================================================================
include(CPM)

find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)

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
  set(SPDLOG_USE_STD_FORMAT ON)
else ()
  message(STATUS "Using external fmt library")
  CPMAddPackage("gh:fmtlib/fmt@12.2.0")
  set(SPDLOG_FMT_EXTERNAL ON)
endif ()

# ---------------------------------------------------------------------------
# spdlog – logging library
# ---------------------------------------------------------------------------
CPMAddPackage("gh:gabime/spdlog@1.17.0")
set_target_properties(spdlog PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

# ---------------------------------------------------------------------------
# cpp-httplib – HTTP client/server
# ---------------------------------------------------------------------------
CPMAddPackage(
    URI     "gh:yhirose/cpp-httplib@0.47.0"
    OPTIONS "HTTPLIB_REQUIRE_OPENSSL ON"
            "HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF"
)
add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)

# ---------------------------------------------------------------------------
# cxxopts – command line parser
# ---------------------------------------------------------------------------
CPMAddPackage(
    URI "gh:jarro2783/cxxopts@3.3.1"
    OPTIONS "CXXOPTS_BUILD_EXAMPLES NO"
            "CXXOPTS_BUILD_TESTS NO"
            "CXXOPTS_ENABLE_INSTALL NO"
)

# ---------------------------------------------------------------------------
# magic_enum – static reflection for enums (header-only)
# ---------------------------------------------------------------------------
CPMAddPackage("gh:Neargye/magic_enum@0.9.8")

# ---------------------------------------------------------------------------
# BS::thread_pool – header-only, no CMakeLists.txt
# ---------------------------------------------------------------------------
CPMAddPackage(
        NAME        thread-pool
        GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
        GIT_TAG     v5.1.0
        DOWNLOAD_ONLY YES
)
if(thread-pool_ADDED)
  add_library(BS_thread_pool INTERFACE)
  target_include_directories(BS_thread_pool INTERFACE ${thread-pool_SOURCE_DIR}/include)
endif()

# ==============================================================================
# Include paths (inherited by driver subdirectory)
# ==============================================================================
include_directories(${PROJECT_SOURCE_DIR}/include/)
include_directories(${PROJECT_SOURCE_DIR}/src/)
include_directories(${CMAKE_BINARY_DIR}/generated/)

# ==============================================================================
# Source files
# ==============================================================================
set(URI src/util/uri.cpp)
set(Config src/config/config.cpp)

set(Updater src/core/updater.cpp)
set(IPUtil src/network/ip_util.cpp src/network/ip_util_http.cpp)
set(Manager src/core/manager.cpp)
set(HttpClient src/network/http_client.cpp)
set(DriverManager src/core/driver_manager.cpp)
set(NetworkManager src/network/network_manager.cpp)
set(CoreLogger src/core/core_logger.cpp)
set(DNS src/dns/dns.cpp src/dns/dns_resolver.cpp src/dns/dns_record_parser.cpp)

# ==============================================================================
# Main executable
# ==============================================================================
add_executable(yaddnsc src/main.cpp src/util/string_util.cpp ${DNS} ${Config} ${DriverManager} ${URI}
  ${Updater} ${Manager} ${IPUtil} ${HttpClient} ${NetworkManager} ${CoreLogger}
)

set_target_properties(yaddnsc PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(yaddnsc PRIVATE BS_thread_pool magic_enum httplib spdlog::spdlog OpenSSL::SSL OpenSSL::Crypto
  ZLIB::ZLIB cxxopts::cxxopts glaze::glaze ${CMAKE_DL_LIBS}
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
# Driver modules
# ==============================================================================
add_subdirectory(${PROJECT_SOURCE_DIR}/driver/)
