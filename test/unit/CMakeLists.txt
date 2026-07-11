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
        ${PROJECT_SOURCE_DIR}/test
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

# record_kind — simple enum, header-only
add_unit_test(record_kind)

# driver_magic — compile-time constant check, header-only
add_unit_test(driver_magic)

# fmt polyfill — header-only, conditional compilation
add_unit_test(fmt_polyfill)

# TtlCache — header-only template, needs pthread for std::thread
add_unit_test(cache)

# bytes — big-endian read helpers, header-only
add_unit_test(bytes)

# random — thread-local RNG engine, header-only
add_unit_test(random)

# fd — RAII UniqueFd wrapper + make_pipe, header-only (POSIX)
add_unit_test(fd)

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

# config_parser — glaze-based JSON config parsing, uses parser.hpp
add_unit_test(config_parser)

# config_validator — config validation detail functions
add_unit_test(config_validator)

# retry_util — retry_on_error with backoff, header-only template
add_unit_test(retry_util)

# resolver_base — ResolverBase interface test, header-only
add_unit_test(resolver_base)

# driver_exceptions — ParamParseException hierarchy test, header-only
add_unit_test(driver_exceptions)

# ============================================================================
#  Tests needing an extra .cpp source (but no external link deps)
# ============================================================================

# scheduler — pure timer-queue logic, needs scheduler.cpp for the impl
add_unit_test(scheduler ${PROJECT_SOURCE_DIR}/src/core/scheduler.cpp)

# updater — needs dispatcher.cpp (ResolverDispatcher) + native DNS parser + updater.cpp
# + inet_address.cpp (in test_support)
# updater — needs dispatcher.cpp (ResolverDispatcher) + native DNS parser + updater.cpp
# + the full IP-source dependency chain (factory, iface, http, mdns, network, http_client, builder).
add_unit_test(updater
    ${PROJECT_SOURCE_DIR}/src/dns/dispatcher.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser/parser_native.cpp
    ${PROJECT_SOURCE_DIR}/src/core/updater.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/factory.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/iface.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/http.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/mdns.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/iface_util.cpp
    ${PROJECT_SOURCE_DIR}/src/network/socket.cpp
    ${PROJECT_SOURCE_DIR}/src/network/socket_addr.cpp
    ${PROJECT_SOURCE_DIR}/src/network/net_devices.cpp
    ${PROJECT_SOURCE_DIR}/src/network/http_client.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp)

# updater — needs dispatcher.cpp (ResolverDispatcher) + native DNS parser + updater.cpp
target_link_libraries(test_updater PRIVATE GTest::gmock glaze::glaze httplib::httplib)
target_compile_definitions(test_updater PRIVATE YADDNSC_USE_NATIVE_DNS=1)
add_unit_test(socket_addr ${PROJECT_SOURCE_DIR}/src/network/socket_addr.cpp)

# ============================================================================
#  ResolverDispatcher — native backend (jthread-based, YADDNSC_USE_NATIVE_DNS=1)
# ============================================================================

add_unit_test(dispatcher
    ${PROJECT_SOURCE_DIR}/src/dns/dispatcher.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser/parser_native.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/error.cpp)
target_link_libraries(test_dispatcher PRIVATE GTest::gmock)
target_compile_definitions(test_dispatcher PRIVATE YADDNSC_USE_NATIVE_DNS=1)

# ============================================================================
#  ResolverDispatcher — legacy system backend (libresolv, default)
# ============================================================================

add_unit_test(dispatcher_system
    ${PROJECT_SOURCE_DIR}/src/dns/dispatcher_system.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser/parser_system.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/error.cpp)
target_link_libraries(test_dispatcher_system PRIVATE GTest::gmock)
if (RESOLV)
  target_link_libraries(test_dispatcher_system PRIVATE ${RESOLV})
endif ()

# ============================================================================
#  Tests needing libresolv (for DNS wire-format construction / parsing)
# ============================================================================

find_library(RESOLV resolv)

# builder_test — tests QueryBuilder (pure logic in builder.cpp, no libresolv)
add_unit_test(builder ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp)

# mkquery_test — tests mkquery_native (uses QueryBuilder) and mkquery_system (uses libresolv)
# query.cpp depends on builder.cpp for QueryBuilder
add_unit_test(mkquery ${PROJECT_SOURCE_DIR}/src/dns/wire/query.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp)
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
add_unit_test(dns_parser_native SOURCE dns_parser.cpp dns_parser_native.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser/parser_native.cpp)
target_compile_definitions(test_dns_parser_native PRIVATE YADDNSC_USE_NATIVE_DNS=1)
