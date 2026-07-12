# ==============================================================================
# Integration tests — end-to-end workflows in a controlled I/O environment
#
# These tests exercise complete workflows (config → resolve → update) using
# real I/O on loopback.  They require Python3 + dnslib for the sim server.
#
# Usage:
#   cmake -B build -DYADDNSC_BUILD_TESTS=ON
#   ninja -C build yaddnsc simple
#   ninja -C build tests          # builds all unit + component tests
#   ctest --test-dir build -R integration  # runs only integration scenarios
# ==============================================================================

# Integration tests are opt-in (they need Python3 + loopback network).
find_package(Python3 COMPONENTS Interpreter QUIET)

if(NOT Python3_FOUND)
    message(STATUS "Integration tests: disabled (Python3 not found)")
    return()
endif()

# These tests spawn a real sim server and exercise the full binary,
# so they depend on yaddnsc and the simple driver being built first.
add_test(NAME integration_scenarios
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_ctest.sh
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

set_tests_properties(integration_scenarios PROPERTIES
    TIMEOUT 120
    RESOURCE_LOCK "integration"
    SKIP_RETURN_CODE 77
    # Debug builds instrument both yaddnsc and dlopen'ed .so files with ASan,
    # causing odr-violation for stdlib globals (unicode_data).  Safely ignored.
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0"
)

# CTest integration: the test runner checks that yaddnsc and the
# simple driver are already built and fails with a clear message.
