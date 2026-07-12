# ==============================================================================
# yaddnsc test suite — CMake build file
#
# Hierarchy:
#   test/
#     CMakeLists.txt       ←  this file — loads GoogleTest, defines test_support
#                                 and the add_unit_test helper
#     unit/                ←  unit-test targets (pure logic, no I/O)
#       common/            ←  cross-module base types
#       config/            ←  configuration tests
#       core/              ←  core logic tests (scheduler, updater)
#       dns/               ←  DNS protocol tests
#       ip_source/         ←  IP-source abstraction tests (header-only)
#       network/           ←  network address/path/URI tests
#       util/              ←  utility tests (string, algorithm, etc.)
#     component/           ←  component tests (real I/O on loopback)
#     mocks/               ←  mock class headers
#     fixtures/            ←  shared test-data builders
#     integration/         ←  integration tests (real I/O in controlled env)
#     perf/                ←  micro-benchmarks (Google Benchmark)
# ==============================================================================

# Clang 19.x needs -fexperimental-library for std::stop_token, std::jthread, etc.
# Test targets compile production source files directly (e.g. scheduler.cpp)
# that depend on these headers, so the flag must apply at the test level too.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19 AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20)
    add_compile_options(-fexperimental-library)
endif()

# Re-use the project's CPM.cmake for GoogleTest.
include(${CMAKE_SOURCE_DIR}/cmake/CPM.cmake)

# ---------------------------------------------------------------------------
# GoogleTest  (v1.17.0 — compatible with C++23, includes gmock)
# ---------------------------------------------------------------------------
# Use the same URI shorthand format as other project dependencies.
CPMAddPackage(
    URI "gh:google/googletest@1.17.0"
    OPTIONS "INSTALL_GTEST OFF"
             "gtest_force_shared_crt ON"
)

if (googletest_ADDED)
    # GoogleTest targets are already defined by the CPM fetch above.
    message(STATUS "GoogleTest v${googletest_VERSION} loaded for test targets")

    # Mark GTest/GMock as SYSTEM to suppress clang-tidy false positives
    # (e.g. bugprone-suspicious-include on gtest-all.cc / gmock-all.cc).
    foreach(tgt gtest gtest_main gmock gmock_main)
      set_target_properties(${tgt} PROPERTIES SYSTEM TRUE)
    endforeach()
endif ()

# ---------------------------------------------------------------------------
# Google Benchmark  (v1.9.5 — micro-benchmark support)
# ---------------------------------------------------------------------------
# Used by test/perf/ benchmarks.  Suppress its own test suite to avoid
# a nested GoogleTest build that would conflict with the one above.
CPMAddPackage(
    URI "gh:google/benchmark@1.9.5"
    OPTIONS "BENCHMARK_ENABLE_TESTING OFF"
            "BENCHMARK_ENABLE_INSTALL OFF"
)

# Google Benchmark's own library must be compiled with optimisations for
# meaningful timing results, even when the parent project is Debug.
if(benchmark_ADDED)
  foreach(tgt benchmark benchmark_main)
    target_compile_options(${tgt} PRIVATE
          $<$<NOT:$<CONFIG:Release>>:-O2 -DNDEBUG>
        )
  endforeach()
endif()

# ---------------------------------------------------------------------------
# test_support  —  static library of production .cpp files needed by tests.
#
# These source files have ZERO project-internal dependencies that would
# require mocking, dlopen, or I/O.  They represent the "pure logic" layer
# that is fully testable without any production code changes.
# ---------------------------------------------------------------------------
add_library(test_support STATIC
    ${PROJECT_SOURCE_DIR}/src/dns/error.cpp
    ${PROJECT_SOURCE_DIR}/src/network/inet_address.cpp
    ${PROJECT_SOURCE_DIR}/src/network/uri.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/validator.cpp
)

target_include_directories(test_support PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/test
    ${PROJECT_BINARY_DIR}/generated
)

target_compile_definitions(test_support PUBLIC
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
    $<$<BOOL:${YADDNSC_USE_STD_FORMAT}>:YADDNSC_USE_STD_FORMAT>
)

# test_support links against the project's fmt polyfill and GTest.
target_link_libraries(test_support PUBLIC
    yaddnsc_fmt
    GTest::gmock
    GTest::gtest
)

# ---------------------------------------------------------------------------
# yaddnsc_test_config  —  light-weight compile config for test targets.
# Unlike yaddnsc_compile_config, this does NOT propagate -Werror or other
# strict compile options, so pre-existing test warnings don't block CI.
# ---------------------------------------------------------------------------
add_library(yaddnsc_test_config INTERFACE)
target_include_directories(yaddnsc_test_config INTERFACE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_BINARY_DIR}/generated
)
target_compile_definitions(yaddnsc_test_config INTERFACE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
    $<$<BOOL:${YADDNSC_USE_STD_FORMAT}>:YADDNSC_USE_STD_FORMAT>
)
target_link_libraries(yaddnsc_test_config INTERFACE
    glaze::glaze
    spdlog::spdlog
    magic_enum
)

# ============================================================================
# Helper macro  add_unit_test(<name> [SOURCE <source>] [extra_sources ...])
#
# Creates:
#   - An executable target  test_<name>
#   - A CTest test          test_<name>
#
#   <name>           — test target name (test_<name>)
#   SOURCE <source>  — explicit test source path (default: ${name}.cpp)
#   extra_sources    — additional .cpp files required by the test
# ============================================================================
function(add_unit_test name)
    cmake_parse_arguments(UT "" "SOURCE" "" ${ARGN})
    if (UT_SOURCE)
        set(test_source ${UT_SOURCE})
    else ()
        set(test_source ${name}.cpp)
    endif ()
    add_executable(test_${name} ${test_source} ${UT_UNPARSED_ARGUMENTS})
    target_link_libraries(test_${name} PRIVATE
        test_support
        yaddnsc_test_config
        GTest::gtest_main
    )
    target_include_directories(test_${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/test
    )
    add_test(NAME test_${name} COMMAND test_${name})
endfunction()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(unit)
add_subdirectory(component)

# Performance benchmarks (require Google Benchmark)
if(benchmark_ADDED)
  add_subdirectory(perf EXCLUDE_FROM_ALL)
endif()

# Integration tests (optional, require controlled I/O environment)
# add_subdirectory(integration)

# Driver-specific tests (require driver source files, no dlopen)
# add_subdirectory(driver)
