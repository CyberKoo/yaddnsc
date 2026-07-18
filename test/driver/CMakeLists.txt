# ==============================================================================
# Driver-specific unit tests
#
# These tests compile the driver source files directly (no dlopen) and exercise
# generate_request, check_response, get_detail, and other public methods.
# ==============================================================================

# CoreLogger implementation needed by driver .cpp files for CORE_LOG_* macros
set(CORE_LOGGER_SRC ${PROJECT_SOURCE_DIR}/src/core/core_logger.cpp)
# Signing implementation needed by signing-enabled drivers (Route53, AlibabaCloud)
set(SIGNING_SRC ${PROJECT_SOURCE_DIR}/src/util/signing.cpp)

# ---------------------------------------------------------------------------
# Helper: add a driver test target
#
#   add_driver_test(<name> <driver_name> [extra_sources ...])
#
# Creates test_driver_<name> compiling <driver_name>_driver_test.cpp plus the
# named driver's <driver_name>.cpp and CoreLogger.  Adds the driver's source
# directory to the include path so config.hpp / response.hpp are found.
# ---------------------------------------------------------------------------
function(add_driver_test name driver_name)
    set(driver_dir ${PROJECT_SOURCE_DIR}/driver/${driver_name})
    add_unit_test(driver_${name}
        SOURCE ${driver_name}_driver_test.cpp
        ${driver_dir}/${driver_name}.cpp
        ${CORE_LOGGER_SRC}
        ${ARGN}
    )
    target_include_directories(test_driver_${name} PRIVATE
        ${driver_dir}
    )
endfunction()

# ============================================================================
# BaseDriver — header-only, tests parse_config<>, get_abi_version, etc.
# ============================================================================
# BaseDriver — header-only but base.h's inline execute() calls CoreLogger
add_unit_test(driver_base SOURCE base_driver_test.cpp ${CORE_LOGGER_SRC})

# ============================================================================
# Individual driver tests  (no special dependencies beyond glaze+spdlog)
# ============================================================================
add_driver_test(simple simple)
add_driver_test(duckdns duckdns)
add_driver_test(cloudflare cloudflare)
add_driver_test(digital_ocean digital_ocean)
add_driver_test(dnspod dnspod ${PROJECT_SOURCE_DIR}/src/network/http_client.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_driver_dnspod PRIVATE httplib OpenSSL::Crypto)
add_driver_test(godaddy godaddy)
add_driver_test(linode linode)
add_driver_test(porkbun porkbun)
add_driver_test(vultr vultr)

# ============================================================================
# AlibabaCloudDriver  —  requires Signing::* implementation (+ OpenSSL)
# ============================================================================
add_driver_test(alibaba_cloud alibaba_cloud ${SIGNING_SRC})
target_link_libraries(test_driver_alibaba_cloud PRIVATE OpenSSL::Crypto)

# ============================================================================
# libxml2-based drivers  —  NamecheapDriver, Route53Driver
# ============================================================================
find_package(LibXml2 QUIET)
if(LibXml2_FOUND)
    # Namecheap — XML response parsing only, no signing
    add_driver_test(namecheap namecheap)
    target_link_libraries(test_driver_namecheap PRIVATE LibXml2::LibXml2)

    # Route53 — XML request body + XML response parsing + AWS SigV4 signing
    add_driver_test(route53 route53 ${SIGNING_SRC})
    target_link_libraries(test_driver_route53 PRIVATE LibXml2::LibXml2 OpenSSL::Crypto)

    message(STATUS "libxml2 found — building namecheap and route53 driver tests")
else()
    message(STATUS "libxml2 not found — skipping namecheap and route53 driver tests")
endif()
