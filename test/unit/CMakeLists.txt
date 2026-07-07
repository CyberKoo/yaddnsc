# ==============================================================================
# Unit tests — zero-change component tests
#
# Each add_unit_test call creates:
#   - An executable target  test_<name>
#   - A CTest test          test_<name>
#
# Helper macro: add_unit_test(<name> [extra_sources ...])
#   <name>           — test file stem (test/unit/<name>.cpp is auto-found)
#   extra_sources    — additional .cpp files required by the test
# ==============================================================================

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
        GTest::gtest_main
        glaze::glaze
        spdlog::spdlog
        magic_enum
    )
    target_include_directories(test_${name} PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_BINARY_DIR}/generated
    )
    add_test(NAME test_${name} COMMAND test_${name})
endfunction()

# ============================================================================
#  Pure header-only / no .cpp dependencies
# ============================================================================

add_unit_test(mixin)
add_unit_test(address_family)
add_unit_test(dns_type)
add_unit_test(http_type)
add_unit_test(algorithm)
add_unit_test(validation)
add_unit_test(dns_util)

# exception tests need exc headers only (compile-time check)
add_unit_test(exception)

# fmt polyfill — header-only, conditional compilation
add_unit_test(fmt_polyfill)

# TtlCache — header-only template, needs pthread for std::thread
add_unit_test(cache)

# ============================================================================
#  Tests requiring test_support .cpp files
# ============================================================================

# dns_error_test — uses dns/error.cpp (already in test_support)
add_unit_test(dns_error)

# inet_address_test — uses inet_address.cpp (already in test_support)
add_unit_test(inet_address)

# uri_test — uses uri.cpp + inet_address.cpp (both in test_support)
add_unit_test(uri)

# dns_validator_test — uses dns/validator.cpp + dns/error.cpp (in test_support)
add_unit_test(dns_validator)

# ============================================================================
#  Header-only tests (no extra .cpp dependencies)
# ============================================================================

# string_util — inline template functions, no .cpp needed
add_unit_test(string_util)

# config_types — header-only struct/enum tests, no .cpp needed
add_unit_test(config_types)

# ============================================================================
#  Tests needing an extra .cpp source (but no external link deps)
# ============================================================================

# socket_addr_test — uses socket_addr.cpp (depends on inet_address, in test_support)
add_unit_test(socket_addr ${PROJECT_SOURCE_DIR}/src/network/socket_addr.cpp)

# ============================================================================
#  Tests needing libresolv (for DNS wire-format construction / parsing)
# ============================================================================

find_library(RESOLV resolv)

# mkquery_test — tests mkquery_manual and mkquery_mdns (pure logic in mkquery.cpp)
# mkquery.cpp also defines mkquery_system() which calls res_mkquery → needs libresolv
add_unit_test(mkquery ${PROJECT_SOURCE_DIR}/src/dns/wire/query.cpp)
if (RESOLV)
  target_link_libraries(test_mkquery PRIVATE ${RESOLV})
endif ()

# dns_parser_system — tests DnsParser (system backend, libresolv-based)
# parser_system.cpp uses ns_initparse / ns_parserr from libresolv
add_unit_test(dns_parser_system SOURCE dns_parser.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser/parser_system.cpp)
if (RESOLV)
  target_link_libraries(test_dns_parser_system PRIVATE ${RESOLV})
endif ()

# dns_parser_native — tests DnsParser (self-contained backend, no libresolv)
# parser_native.cpp implements the full DNS wire-format parser without system libraries.
# The test file dns_parser.cpp includes dns/parser/parser.h which conditionally
# selects the parser backend via YADDNSC_USE_NATIVE_DNS.  We override it per-target
# so this test always exercises the native path regardless of the project default.
add_unit_test(dns_parser_native SOURCE dns_parser.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser/parser_native.cpp)
target_compile_definitions(test_dns_parser_native PRIVATE YADDNSC_USE_NATIVE_DNS=1)
