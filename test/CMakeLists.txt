# ==============================================================================
# yaddnsc test suite — CMake build file
#
# Hierarchy:
#   test/
#     CMakeLists.txt       ←  this file — loads GoogleTest, defines test_support
#     unit/CMakeLists.txt  ←  unit-test targets (pure logic, no I/O)
#     mocks/               ←  mock class headers (used by unit/ and driver/ tests)
#     fixtures/            ←  shared test-data builders
#     integration/         ←  integration tests (real I/O in a controlled env)
#     driver/              ←  driver-specific tests (no dlopen required)
#     perf/                ←  micro-benchmarks (Google Benchmark)
# ==============================================================================

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
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(unit)

# Integration tests (optional, require controlled I/O environment)
# add_subdirectory(integration)

# Driver-specific tests (require driver source files, no dlopen)
# add_subdirectory(driver)

# Performance benchmarks (require Google Benchmark)
# add_subdirectory(perf)
