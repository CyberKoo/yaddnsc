# ==============================================================================
# Driver-specific unit tests
#
# These tests compile the driver source files directly (no dlopen) and drive
# them through their C entry points with FakeHostServices (abi_test_harness.h).
# ==============================================================================

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
    target_include_directories(test_driver_${name} PRIVATE
        ${driver_dir}
        ${PROJECT_SOURCE_DIR}/plugin_crypto
    )
endfunction()

# ============================================================================
# Individual driver tests  (no special dependencies beyond glaze+spdlog)
# ============================================================================
add_abi_driver_test(simple simple)
add_abi_driver_test(duckdns duckdns)
add_abi_driver_test(cloudflare cloudflare)
add_abi_driver_test(digital_ocean digital_ocean)
add_abi_driver_test(dnspod dnspod)
add_abi_driver_test(godaddy godaddy)
add_abi_driver_test(linode linode)
add_abi_driver_test(porkbun porkbun)
add_abi_driver_test(vultr vultr)

# ============================================================================
# AlibabaCloudDriver  —  requires Signing::* implementation (+ OpenSSL)
# ============================================================================
add_abi_driver_test(alibaba_cloud alibaba_cloud)
target_link_libraries(test_driver_alibaba_cloud PRIVATE yaddnsc_plugin_crypto OpenSSL::Crypto)

# ============================================================================
# libxml2-based drivers  —  NamecheapDriver, Route53Driver
# ============================================================================
find_package(LibXml2 QUIET)
if(LibXml2_FOUND)
    # Namecheap — XML response parsing only, no signing
    add_abi_driver_test(namecheap namecheap)
    target_link_libraries(test_driver_namecheap PRIVATE LibXml2::LibXml2)

    # Route53 — XML request body + XML response parsing + AWS SigV4 signing
    add_abi_driver_test(route53 route53)
    target_link_libraries(test_driver_route53 PRIVATE yaddnsc_plugin_crypto LibXml2::LibXml2 OpenSSL::Crypto)

    message(STATUS "libxml2 found — building namecheap and route53 driver tests")
else()
    message(STATUS "libxml2 not found — skipping namecheap and route53 driver tests")
endif()
