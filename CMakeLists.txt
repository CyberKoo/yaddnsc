cmake_minimum_required(VERSION 3.28...3.31 FATAL_ERROR)
project(yaddnsc VERSION 1.1.0)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# Project configuration.
include(ProjectSetup)

# Third-party dependencies, then first-party build configuration.
include(Dependencies)
include(BuildOptions)

# Host modules and executable.
include(ProductionTargets)
include(Executable)

# Build configuration summary.
message(STATUS "Minimum update interval: ${YADDNSC_MIN_UPDATE_INTERVAL}s")

if (YADDNSC_USE_SYSTEM_SPDLOG)
  message(STATUS "spdlog: system package")
else ()
  message(STATUS "spdlog: bundled (CPM)")
endif ()

message(STATUS "Default DNS server: ${YADDNSC_DEFAULT_DNS_SERVER}:${YADDNSC_DEFAULT_DNS_PORT}")

# Documentation.
include(Doxygen)

# Plugin SDK and bundled drivers.
include(PluginSdk)
add_subdirectory(${PROJECT_SOURCE_DIR}/driver/)

# Header self-containment checks. Release builds compile only production targets.
if (NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  include(HeaderCheck)
endif ()

# Optional test suite.
option(YADDNSC_BUILD_TESTS "Build unit and integration tests" OFF)
mark_as_advanced(YADDNSC_BUILD_TESTS)

if (YADDNSC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif ()

# Installation and packaging.
include(InstallRules)

# Generated headers come last: paths baked into them (the default driver
# directory in config_cmake.h) follow the packaging configuration set up by
# InstallRules/Packaging.
include(GeneratedHeaders)
