cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(yaddnsc)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

option("LIBC_MUSL" "OFF")
option("NO_RTTI" "OFF")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb -gdwarf-3 -Wall")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/objs/lib)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/objs)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# yaddnsc version
set(YADDNSC_VERSION_MAJOR 0)
set(YADDNSC_VERSION_MINOR 2)
set(YADDNSC_VERSION_PATCH 0)
set(YADDNSC_DEVELOPMENT ON)
set(YADDNSC_RELEASE_INFO RELEASE)
set(YADDNSC_DRV_VERSION "1000000")

if (YADDNSC_DEVELOPMENT)
    include(GitVersion)
    set(YADDNSC_RELEASE_INFO ${GIT_VERSION})
endif ()

# if not musl add lto flag to release target
if (NOT LIBC_MUSL)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto=auto")
endif ()

# disable rtti
if (NO_RTTI)
    message(STATUS "RTTI disabled")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
    add_compile_definitions(CXXOPTS_NO_RTTI)
endif ()

# set default build target to release
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

# include headers
include_directories(${PROJECT_SOURCE_DIR}/include/)
include_directories(${PROJECT_SOURCE_DIR}/deps/thread-pool/include)

## required modules ##
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
## required modules ##

# check symbols
include(CheckSymbolExists)
include(CheckStructHasMember)
check_symbol_exists(res_nquery "netinet/in.h;resolv.h" HAVE_RES_NQUERY)
check_symbol_exists(res_setservers "netinet/in.h;resolv.h" HAVE_RES_SETSERVERS)
check_symbol_exists(res_ndestroy "netinet/in.h;resolv.h" HAVE_RES_NDESTROY)
check_struct_has_member("struct __res_state" _u._ext.nsaddrs "netinet/in.h;resolv.h" HAVE_RES_STATE_EXT_NSADDRS LANGUAGE C)

# generate header file for .h.in file
include_directories(${CMAKE_BINARY_DIR}/generated/)
configure_file("${CMAKE_SOURCE_DIR}/include/version.h.in" "${CMAKE_BINARY_DIR}/generated/version.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/include/driver_ver.h.in" "${CMAKE_BINARY_DIR}/generated/driver_ver.h" @ONLY)
configure_file("${CMAKE_SOURCE_DIR}/include/config_cmake.h.in" "${CMAKE_BINARY_DIR}/generated/config_cmake.h")

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
add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

# cpp-httplib
add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)
set(HTTPLIB_REQUIRE_OPENSSL ON)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF)
add_subdirectory(deps/cpp-httplib EXCLUDE_FROM_ALL)

# cxxopts command line parser
add_subdirectory(deps/cxxopts EXCLUDE_FROM_ALL)
## submodules ##

### source code
set(DNS src/dns.cpp src/dns.h)
set(URI src/uri.cpp include/uri.h)
set(Config src/config.cpp src/config.h)
set(Worker src/worker.cpp src/worker.h)
set(IPUtil src/ip_util.cpp src/ip_util.h)
set(Context src/context.cpp src/context.h)
set(Manager src/manager.cpp src/manager.h)
set(HttpClient src/httpclient.cpp include/httpclient.h)
set(DriverManager src/driver_manager.cpp src/driver_manager.h)
set(SignalHandler src/signal_handler.cpp src/signal_handler.h)

# library
add_library(string_util STATIC src/string_util.cpp include/string_util.h)
add_library(network_util STATIC src/network_util.cpp include/network_util.h)
set_target_properties(string_util PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(network_util PROPERTIES POSITION_INDEPENDENT_CODE ON)

# main executable
add_executable(yaddnsc src/main.cpp ${DNS} ${Context} ${Config} ${DriverManager} ${URI} ${Worker} ${Manager} ${IPUtil}
        ${HttpClient} ${SignalHandler})

# link internal libraries
target_link_libraries(yaddnsc PRIVATE string_util network_util)

# link third party libraries
target_link_libraries(yaddnsc PRIVATE httplib spdlog::spdlog fmt::fmt OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB
        cxxopts::cxxopts nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})

# libresolv is needed on linux, but not on BSDs, so only link it if we can find it
find_library(RESOLV resolv)
if (RESOLV)
    target_link_libraries(yaddnsc PRIVATE ${RESOLV})
endif ()

# external drivers
add_subdirectory(${PROJECT_SOURCE_DIR}/driver/)
