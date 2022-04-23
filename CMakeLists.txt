cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(yaddnsc)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

option("LIBC_MUSL" "OFF")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -g -ggdb -gdwarf-3 -Wall")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/objs/lib)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/objs)

# yaddnsc version
set(YADDNSC_VERSION_MAJOR 0)
set(YADDNSC_VERSION_MINOR 1)
set(YADDNSC_VERSION_PATCH 5)
set(YADDNSC_DRV_VERSION "1000000")

# if not musl add lto flag to release target
if (NOT LIBC_MUSL)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto")
endif ()

# set default build target to release
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# include headers
include_directories(${PROJECT_SOURCE_DIR}/include/)

# generate header file for .h.in file
include_directories(${CMAKE_BINARY_DIR}/generated/)
configure_file("${CMAKE_SOURCE_DIR}/include/version.h.in" "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/include/driver_ver.h.in" "${CMAKE_BINARY_DIR}/generated/driver_ver.h" @ONLY)

## required modules ##
find_package(Filesystem REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
## required modules ##

# check symbols
include(CheckSymbolExists)
check_symbol_exists("res_nquery" "resolv.h" "HAVE_RES_NQUERY")
if (${HAVE_RES_NQUERY})
    add_compile_definitions(USE_RES_NQUERY)
endif ()

## submodules ##
# nlohmann_json
set(JSON_BuildTests OFF)
set(JSON_Install OFF)
set(JSON_MultipleHeaders ON)
add_subdirectory(deps/nlohmann_json EXCLUDE_FROM_ALL)

# fmt
add_subdirectory(deps/fmt EXCLUDE_FROM_ALL)
set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON)

# spdlog
set(SPDLOG_FMT_EXTERNAL ON)
add_subdirectory(deps/spdlog EXCLUDE_FROM_ALL)
set_target_properties(spdlog PROPERTIES POSITION_INDEPENDENT_CODE ON)
# define spdlog logging level
add_compile_definitions(SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG)

# cpp-httplib
add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)
set(HTTPLIB_REQUIRE_OPENSSL ON)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF)
add_subdirectory(deps/cpp-httplib EXCLUDE_FROM_ALL)

# cxxopts command line parser
add_subdirectory(deps/cxxopts EXCLUDE_FROM_ALL)

# magic enum
add_subdirectory(deps/magic_enum EXCLUDE_FROM_ALL)
## submodules ##

### source code
set(Context src/context.cpp src/context.h)
set(Manager src/manager.cpp src/manager.h)
set(Config src/config.cpp src/config.h)
set(DriverManager src/driver_manager.cpp src/driver_manager.h)
set(Worker src/worker.cpp src/worker.h)
set(NetworkUtil src/network_util.cpp include/network_util.h)
set(DNS src/dns.cpp src/dns.h)
set(URI src/uri.cpp include/uri.h)
set(IPUtil src/ip_util.cpp src/ip_util.h)
set(HttpClient src/httpclient.cpp include/httpclient.h)

# library
add_library(string_util STATIC src/string_util.cpp include/string_util.h)

# main executable
add_executable(yaddnsc src/main.cpp ${Context} ${Config} ${DriverManager} ${URI} ${DNS} ${Worker} ${Manager}
        ${NetworkUtil} ${HttpClient} ${IPUtil})

# link internal library
target_link_libraries(yaddnsc PRIVATE string_util)

target_link_libraries(yaddnsc PRIVATE httplib spdlog::spdlog fmt::fmt OpenSSL::SSL OpenSSL::Crypto magic_enum::magic_enum
        ZLIB::ZLIB cxxopts::cxxopts std::filesystem nlohmann_json::nlohmann_json resolv ${CMAKE_DL_LIBS})

# external drivers
add_subdirectory(${PROJECT_SOURCE_DIR}/driver/)
