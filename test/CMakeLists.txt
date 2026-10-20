# ==============================================================================
# yaddnsc test suite — CMake build file
#
# Hierarchy:
#   test/
#     CMakeLists.txt       ←  this file — loads GoogleTest, defines test_support
#                                 and the add_unit_test helper
#     unit/                ←  unit-test targets (pure logic, no I/O); the layout
#                                 mirrors src/: domain/, application/, coro/,
#                                 infrastructure/<module>/, support/, plus sdk/
#                                 (public plugin headers) and plugin_support/
#     component/           ←  component tests (real I/O on loopback)
#     mocks/               ←  mock class headers
#     fixtures/            ←  shared test-data builders
#     integration/         ←  integration tests (real I/O in controlled env)
#     perf/                ←  micro-benchmarks (Google Benchmark)
#     plugin/              ←  v1 plugin ABI contract tests (dlopen)
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
    # Also exempt them from IWYU: they are third-party TUs and must not fail
    # the enforced include-hygiene check.
    foreach(tgt gtest gtest_main gmock gmock_main)
      set_target_properties(${tgt} PROPERTIES SYSTEM TRUE CXX_INCLUDE_WHAT_YOU_USE "")
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
# Also exempt it from IWYU: it is a third-party TU and must not fail the
# enforced include-hygiene check.
if(benchmark_ADDED)
  foreach(tgt benchmark benchmark_main)
    set_target_properties(${tgt} PROPERTIES CXX_INCLUDE_WHAT_YOU_USE "")
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
    ${PROJECT_SOURCE_DIR}/src/domain/error/dns_error.cpp
    ${PROJECT_SOURCE_DIR}/src/domain/network/inet_address.cpp
    ${PROJECT_SOURCE_DIR}/src/domain/update/update_decision.cpp
    ${PROJECT_SOURCE_DIR}/src/domain/dns/record_kind.cpp
    ${PROJECT_SOURCE_DIR}/src/domain/address_policy.cpp
    ${PROJECT_SOURCE_DIR}/src/domain/fqdn.cpp
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
# test_allocation_fault  —  allocation-fault injection for fault-injection
# test binaries (mocks/allocation_fault.h).
#
# The replaced global operator new/delete family must live in its own
# translation unit: inlined into test code it trips GCC's
# -Wmismatched-new-delete under -Werror, and partially replaced families
# break ASan's alloc/dealloc pairing. An OBJECT library is used so the
# replacement object is always linked in whole, never dropped by archive
# member extraction.
# ---------------------------------------------------------------------------
add_library(test_allocation_fault OBJECT mocks/allocation_fault.cpp)
target_include_directories(test_allocation_fault PRIVATE
    ${PROJECT_SOURCE_DIR}/test
)
target_link_libraries(test_allocation_fault PRIVATE yaddnsc_warnings)

# ---------------------------------------------------------------------------
# test_main  —  shared entry point for every test binary, replacing
# gtest_main. Exits 77 when any case was skipped and nothing failed, which
# add_unit_test pairs with SKIP_RETURN_CODE so CTest marks the binary as
# Skipped rather than Passed. A skip means the environment lacked something
# the test needed; that must stay visible instead of counting as a pass.
# ---------------------------------------------------------------------------
add_library(test_main STATIC test_main.cpp)
target_link_libraries(test_main PRIVATE GTest::gtest)

# ---------------------------------------------------------------------------
# yaddnsc_test_config  —  compile config for test targets.
# Test targets inherit the same strict warnings as production code
# (via yaddnsc_warnings); pre-existing test warnings must be fixed,
# not silenced.
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
    yaddnsc_warnings
    glaze::glaze
    spdlog::spdlog
    magic_enum
)

# ---------------------------------------------------------------------------
# yaddnsc_prod_all  —  all production module targets, bundled for test
# targets that exercise the full application stack.
#
# Architecture rule: test targets link the production module targets
# instead of compiling hand-picked production .cpp files.
#
# The production modules are static libraries built with sanitizer
# instrumentation, so consumers must be given sanitizer link flags —
# add_unit_test does this unconditionally.
# ---------------------------------------------------------------------------
add_library(yaddnsc_prod_all INTERFACE)
target_link_libraries(yaddnsc_prod_all INTERFACE
    yaddnsc_config_infrastructure
    yaddnsc_domain
    yaddnsc_application
    yaddnsc_plugin_loader_adapter
    yaddnsc_logging_adapter
    yaddnsc_plugin_infrastructure
    yaddnsc_cli_adapter
    yaddnsc_composition
    yaddnsc_dns_classic
    yaddnsc_ip_source_infrastructure
    yaddnsc_net
    yaddnsc_coro
    yaddnsc_dns_bootstrap
    yaddnsc_http
    yaddnsc_coro_dns
    yaddnsc_coro_application
    yaddnsc_coro_ip_source
    yaddnsc_coro_plugin
)

# ---------------------------------------------------------------------------
# YADDNSC_TEST_ASAN_OPTIONS — shared ASan runtime options for tests that
# dlopen sanitizer-instrumented modules (drivers, plugin fixtures).
#
# - detect_odr_violation=0: ASan's ODR checker reports a plugin's vtables as
#   duplicate-registration conflicts — a known false positive; a plugin is a
#   separate module by design.
# - report_globals=0 (macOS only): dlclose poisons the unloaded image's
#   global redzones, and dyld reuses the address range for the next dlopen;
#   registering that image runs libmacho's getsectiondata, whose intercepted
#   strcmp/strncmp reads load commands whose shadow still carries the stale
#   poison — a global-buffer-overflow false positive that aborts the test
#   process. Apple ASan gates strcmp behind intercept_strcmp but exposes no
#   switch for strncmp, so global-overflow reporting is disabled for these
#   binaries instead.
# ---------------------------------------------------------------------------
set(YADDNSC_TEST_ASAN_OPTIONS "ASAN_OPTIONS=detect_odr_violation=0")
if(APPLE)
    string(APPEND YADDNSC_TEST_ASAN_OPTIONS ":report_globals=0")
endif()
string(APPEND YADDNSC_TEST_ASAN_OPTIONS ":detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")

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
        test_main
    )
    # Production module targets are sanitizer-instrumented; every test binary
    # gets matching flags so linking any of them (and dlopen'ing sanitized
    # driver .so files) works uniformly.
    add_sanitizer_flags(test_${name})
    target_include_directories(test_${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/test
    )
    # --gtest_shuffle runs the cases in a random order. Without it an implicit
    # dependency on an earlier case is invisible: a test that only passes
    # because it happens to run first stays green forever, and breaks the next
    # time someone reorders the file, narrows a --gtest_filter, or adds a case.
    # GoogleTest prints the seed it used, so a failure replays with
    # --gtest_random_seed=<n>. State that must be process-wide (signal masks,
    # memoised CA-bundle lookups) belongs in its own executable; see
    # component/cert_util_override_test.cpp.
    add_test(NAME test_${name} COMMAND test_${name} --gtest_shuffle)
    # test_main exits 77 when cases were skipped (and nothing failed); surface
    # that as a Skipped test instead of letting it count as a pass.
    set_tests_properties(test_${name} PROPERTIES SKIP_RETURN_CODE 77)

    # Collect all test targets for the top-level test-build convenience target.
    set_property(GLOBAL APPEND PROPERTY YADDNSC_TEST_TARGETS test_${name})
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

# Integration tests (optional, require Python3 + loopback network)
add_subdirectory(integration)

# Driver-specific tests (require driver source files, no dlopen)
add_subdirectory(driver)

# Plugin contract tests (v1 C ABI; dlopen a whiteboard test plugin)
add_subdirectory(plugin)

# ============================================================================
# architecture_guard — static boundary checks (include hygiene for
# domain/application, plugin surface purity, no host-symbol backfill).
# Textual checks only; see cmake/ArchitectureGuard.cmake.
# ============================================================================
add_test(NAME architecture_guard
    COMMAND ${CMAKE_COMMAND}
        -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${PROJECT_SOURCE_DIR}/cmake/ArchitectureGuard.cmake
)

# ============================================================================
# tests  —  top-level convenience target to build all test executables
#           without building the main yaddnsc binary.
#
# Usage:
#   ninja -C build tests
#   ctest --test-dir build
# ============================================================================
get_property(all_test_targets GLOBAL PROPERTY YADDNSC_TEST_TARGETS)

# The 'tests' target also needs yaddnsc (for integration tests) and the driver
# shared libraries (for driver_loader tests) to be built.
add_custom_target(tests DEPENDS ${all_test_targets} yaddnsc drivers)

# Install headers/metadata, then consume them outside the host build graph.
add_test(NAME sdk_install_consumer COMMAND ${CMAKE_COMMAND}
    -DBUILD_DIR=${PROJECT_BINARY_DIR} -DWORK_DIR=${PROJECT_BINARY_DIR}/sdk-consumer-check
    -DVERIFY_OLD_CXX_ABI=$<STREQUAL:${CMAKE_CXX_COMPILER_ID},GNU>
    -P ${PROJECT_SOURCE_DIR}/test/sdk_consumer/install_verify.cmake)
set_tests_properties(sdk_install_consumer PROPERTIES TIMEOUT 600)
