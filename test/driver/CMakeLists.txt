# ==============================================================================
# Driver-specific unit tests
#
# These tests compile the driver source files directly (no dlopen) and drive
# them through their C entry points with FakeHostServices (abi_test_harness.h).
# Every bundled driver gets a test; the list comes from cmake/Drivers.cmake.
# ==============================================================================

include(Drivers)

# ---------------------------------------------------------------------------
# Helper: add an ABI-style driver test target (v1 alpha plugin interface)
#
#   add_abi_driver_test(<name> <driver_name> [extra_sources ...])
#
# Creates test_driver_<name> compiling <driver_name>_driver_test.cpp plus the
# named driver's <driver_name>.cpp, linked against the SDK (no CoreLogger, no
# host-internal sources). The test drives the driver through its C entry
# points with FakeHostServices (abi_test_harness.h).
# ---------------------------------------------------------------------------
function(add_abi_driver_test name driver_name)
    set(driver_dir ${PROJECT_SOURCE_DIR}/driver/${driver_name})
    add_unit_test(driver_${name}
        SOURCE ${driver_name}_driver_test.cpp
        ${driver_dir}/${driver_name}.cpp
        ${ARGN}
    )
    target_link_libraries(test_driver_${name} PRIVATE yaddnsc_plugin_sdk)
    target_include_directories(test_driver_${name} PRIVATE ${driver_dir})
endfunction()

# ============================================================================
#  One test per bundled driver
# ============================================================================
foreach (DRIVER_NAME ${YADDNSC_DRIVERS})
    if ((DRIVER_NAME STREQUAL "namecheap" OR DRIVER_NAME STREQUAL "route53")
        AND NOT YADDNSC_HAS_SDK_XML_COMPONENT)
        # libxml2-based drivers build only with the SDK XML component.
        continue ()
    endif ()
    add_abi_driver_test(${DRIVER_NAME} ${DRIVER_NAME})
endforeach ()

# ============================================================================
#  Extra per-driver dependencies
# ============================================================================

# AlibabaCloudDriver — requires Signing::* implementation (+ OpenSSL)
target_link_libraries(test_driver_alibaba_cloud PRIVATE yaddnsc_plugin_support_crypto OpenSSL::Crypto)

if(YADDNSC_HAS_SDK_XML_COMPONENT)
    # Namecheap — XML response parsing only, no signing
    target_link_libraries(test_driver_namecheap PRIVATE yaddnsc_plugin_sdk_xml)

    # Route53 — XML request body + XML response parsing + AWS SigV4 signing
    target_link_libraries(test_driver_route53 PRIVATE yaddnsc_plugin_sdk_xml yaddnsc_plugin_support_crypto
        OpenSSL::Crypto)
else()
    message(STATUS "libxml2 not found — skipping namecheap and route53 driver tests")
endif()
