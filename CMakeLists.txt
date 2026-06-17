cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(yaddnsc)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# --- compiler & project-wide settings ---
option(LIBC_MUSL "Use musl libc" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb -gdwarf-3 -Wall")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/objs/lib)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/objs)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# if not musl add lto flag to release target
if (NOT LIBC_MUSL)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto=auto")
endif ()

# set default build target to release
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# --- feature detection ---
include(FeatureCheck)

# --- version info ---
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

# --- generated headers ---
include_directories(${CMAKE_BINARY_DIR}/generated/)
configure_file("${CMAKE_SOURCE_DIR}/include/version.h.in" "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/include/driver_ver.h.in" "${CMAKE_BINARY_DIR}/generated/driver_ver.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/include/config_cmake.h.in" "${CMAKE_BINARY_DIR}/generated/config_cmake.h")

# --- required system libraries ---
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)

# --- third-party dependencies ---
include(Dependencies)

# --- source file variables ---
include_directories(${PROJECT_SOURCE_DIR}/include/)

set(URI src/uri.cpp)
set(Config src/config.cpp)
set(Worker src/worker.cpp)
set(IPUtil src/ip_util.cpp)
set(Manager src/manager.cpp)
set(HttpClient src/httpclient.cpp)
set(DriverManager src/driver_manager.cpp)
set(SignalHandler src/signal_handler.cpp)
set(NetworkManager src/network_manager.cpp)
set(CoreLogger src/core_logger.cpp)
set(DNS src/dns.cpp src/dns_resolver.cpp src/dns_record_parser.cpp)

# --- static libraries ---
add_library(string_util STATIC src/string_util.cpp)
set_target_properties(string_util PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- main executable ---
add_executable(yaddnsc src/main.cpp ${DNS} ${Context} ${Config} ${DriverManager} ${URI} ${Worker} ${Manager} ${IPUtil}
        ${HttpClient} ${SignalHandler} ${NetworkManager} ${CoreLogger})

# link internal libraries
target_link_libraries(yaddnsc PRIVATE string_util BS_thread_pool)

# link third party libraries
target_link_libraries(yaddnsc PRIVATE httplib spdlog::spdlog yaddnsc_fmt OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB
        cxxopts::cxxopts glaze::glaze ${CMAKE_DL_LIBS})

# libresolv is needed on linux, but not on BSDs, so only link it if we can find it
find_library(RESOLV resolv)
if (RESOLV)
    target_link_libraries(yaddnsc PRIVATE ${RESOLV})
endif ()

# --- external driver modules ---
add_subdirectory(${PROJECT_SOURCE_DIR}/driver/)
