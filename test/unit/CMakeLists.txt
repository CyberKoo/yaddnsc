# ==============================================================================
# Unit tests — pure logic, no I/O
#
# All tests in this directory exercise only in-memory logic:
#   - header-only template instantiations
#   - value-type constructors / accessors / predicates
#   - pure-algorithm functions (string split/trim, bytes, base64, etc.)
#   - compile-time constant / trait checks
#
# Tests that require real I/O (loopback sockets, filesystem, network) belong
# in ../component/ instead.
# ==============================================================================

# The add_unit_test macro is inherited from ../CMakeLists.txt.
# Each call creates:
#   - An executable target  test_<name>
#   - A CTest test          test_<name>

# ============================================================================
#  util/  —  algorithms, containers, formatting, platform wrappers
# ============================================================================

add_unit_test(algorithm     SOURCE util/algorithm_test.cpp)
add_unit_test(bytes         SOURCE util/bytes_test.cpp)
add_unit_test(cache         SOURCE util/cache_test.cpp)
add_unit_test(fd            SOURCE util/fd_test.cpp)
add_unit_test(fmt_polyfill  SOURCE util/fmt_polyfill_test.cpp)
add_unit_test(mixin         SOURCE util/mixin_test.cpp)
add_unit_test(random        SOURCE util/random_test.cpp)
add_unit_test(retry_util    SOURCE util/retry_util_test.cpp)
add_unit_test(string_util   SOURCE util/string_util_test.cpp)
add_unit_test(validation    SOURCE util/validation_test.cpp)

# ============================================================================
#  common/  —  cross-module base types (address_family, exceptions, etc.)
# ============================================================================

add_unit_test(address_family SOURCE common/address_family_test.cpp)
add_unit_test(exception      SOURCE common/exception_test.cpp)
add_unit_test(http_type      SOURCE common/http_type_test.cpp)
add_unit_test(resolver_base  SOURCE common/resolver_base_test.cpp)

# ============================================================================
#  config/  —  configuration parsing, types, and validation
# ============================================================================

add_unit_test(config_parser     SOURCE config/config_parser_test.cpp)
add_unit_test(config_types      SOURCE config/config_types_test.cpp)
add_unit_test(config_validator  SOURCE config/config_validator_test.cpp
    ${PROJECT_SOURCE_DIR}/src/network/net_devices.cpp)

# ============================================================================
#  core/  —  scheduler (pure timer queue), updater (ResolverDispatcher)
# ============================================================================

add_unit_test(scheduler SOURCE core/scheduler_test.cpp
    ${PROJECT_SOURCE_DIR}/src/core/scheduler.cpp)

# updater — requires the full DNS + IP-source dependency chain
add_unit_test(updater SOURCE core/updater_test.cpp
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
target_link_libraries(test_updater PRIVATE GTest::gmock glaze::glaze httplib::httplib)
target_compile_definitions(test_updater PRIVATE YADDNSC_USE_NATIVE_DNS=1)

# ============================================================================
#  dns/  —  DNS protocol types, parser, dispatchers, wire-format builders
# ============================================================================

# dns_error — uses dns/error.cpp (already in test_support)
add_unit_test(dns_error    SOURCE dns/dns_error_test.cpp)
add_unit_test(dns_type     SOURCE dns/dns_type_test.cpp)
add_unit_test(dns_util     SOURCE dns/dns_util_test.cpp)
add_unit_test(record_kind  SOURCE dns/record_kind_test.cpp)

# dns_validator — uses dns/validator.cpp + dns/error.cpp (in test_support)
add_unit_test(dns_validator SOURCE dns/dns_validator_test.cpp)

# ResolverDispatcher — native backend (jthread-based, YADDNSC_USE_NATIVE_DNS=1)
add_unit_test(dispatcher SOURCE dns/dispatcher_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/dispatcher.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser/parser_native.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/error.cpp)
target_link_libraries(test_dispatcher PRIVATE GTest::gmock)
target_compile_definitions(test_dispatcher PRIVATE YADDNSC_USE_NATIVE_DNS=1)

# wire-format builders and queries
add_unit_test(builder SOURCE dns/builder_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp)

# build_query tests
add_unit_test(mkquery SOURCE dns/mkquery_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp)

# RecordParser — native backend (self-contained, no libresolv)
add_unit_test(dns_parser_native SOURCE dns/parser_test.cpp dns/parser_native_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser/parser_native.cpp)
target_compile_definitions(test_dns_parser_native PRIVATE YADDNSC_USE_NATIVE_DNS=1)

# ============================================================================
#  network/  —  network address/path/URI parsing (pure logic, no I/O)
# ============================================================================

# inet_address — uses inet_address.cpp (already in test_support)
add_unit_test(inet_address SOURCE network/inet_address_test.cpp)

# socket_addr — value-type wrapper around sockaddr_storage
add_unit_test(socket_addr SOURCE network/socket_addr_test.cpp
    ${PROJECT_SOURCE_DIR}/src/network/socket_addr.cpp)

# URI parsing — uses uri.cpp + inet_address.cpp (in test_support)
add_unit_test(uri_parse SOURCE network/uri_parse_test.cpp)
add_unit_test(uri_query SOURCE network/uri_query_test.cpp)
add_unit_test(uri_encode SOURCE network/uri_encode_test.cpp)
add_unit_test(uri_edge   SOURCE network/uri_edge_test.cpp)

# ============================================================================
#  ip_source/  —  IP-source abstraction (header-only tests only)
# ============================================================================

# driver_magic — compile-time constant check, header-only
add_unit_test(driver_magic SOURCE ip_source/driver_magic_test.cpp)

# driver_exceptions — ParamParseException hierarchy test, header-only
add_unit_test(driver_exceptions SOURCE ip_source/driver_exceptions_test.cpp)
