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
add_unit_test(cancellation_token SOURCE util/cancellation_token_test.cpp)
add_unit_test(fd            SOURCE util/fd_test.cpp)
add_unit_test(fmt_polyfill  SOURCE util/fmt_polyfill_test.cpp)
add_unit_test(http_fmt      SOURCE util/http_fmt_test.cpp)
add_unit_test(mixin         SOURCE util/mixin_test.cpp)
add_unit_test(random        SOURCE util/random_test.cpp)
target_link_libraries(test_random PRIVATE OpenSSL::Crypto)
add_unit_test(retry_util    SOURCE util/retry_util_test.cpp)
add_unit_test(string_util   SOURCE util/string_util_test.cpp)
add_unit_test(validation    SOURCE util/validation_test.cpp)

# signing — cryptographic primitives (SHA, HMAC, Base64, hex)
add_unit_test(signing SOURCE util/signing_test.cpp
    ${PROJECT_SOURCE_DIR}/src/util/signing.cpp)
target_link_libraries(test_signing PRIVATE OpenSSL::Crypto)

# ============================================================================
#  common/  —  cross-module base types (address_family, exceptions, etc.)
# ============================================================================

add_unit_test(address_family SOURCE common/address_family_test.cpp)
add_unit_test(exception      SOURCE common/exception_test.cpp)
add_unit_test(resolver_base  SOURCE common/resolver_base_test.cpp)

# ============================================================================
#  config/  —  configuration parsing, types, and validation
# ============================================================================

add_unit_test(config_parser     SOURCE config/config_parser_test.cpp)
add_unit_test(config_types      SOURCE config/config_types_test.cpp)
add_unit_test(config_validator  SOURCE config/config_validator_test.cpp
    ${PROJECT_SOURCE_DIR}/src/network/net_devices.cpp)

# config_loader — JSON config file parsing
add_unit_test(config_loader SOURCE config/config_loader_test.cpp
    ${PROJECT_SOURCE_DIR}/src/config/config.cpp)

# ============================================================================
#  core/  —  scheduler (pure timer queue), updater (ResolverDispatcher)
# ============================================================================

add_unit_test(scheduler SOURCE core/scheduler_test.cpp
    ${PROJECT_SOURCE_DIR}/src/core/scheduler.cpp)

# signal_watcher — install/construct/destroy lifecycle (signals are blocked
# process-wide by install(), so this binary only runs these tests).
add_unit_test(signal_watcher SOURCE core/signal_watcher_test.cpp
    ${PROJECT_SOURCE_DIR}/src/core/signal_watcher.cpp)

# driver_loader + driver_manager — dlopen-based driver loading
# Fixture: a shared library that is deliberately NOT a yaddnsc driver —
# verifies that auto-discovery skips foreign libraries instead of aborting.
# MODULE produces a .so on both Linux and macOS (SHARED would be .dylib).
add_library(yaddnsc_bad_driver_fixture MODULE core/bad_driver_fixture.cpp)

# Fixture: a library that passes magic/compiler verification but whose
# create() returns nullptr — exercises the null-instance guard in
# DriverModule::create_driver. Must use the project compile config so the
# compiler-identity hash matches the host's.
add_library(yaddnsc_null_driver_fixture MODULE core/null_driver_fixture.cpp)
target_link_libraries(yaddnsc_null_driver_fixture PRIVATE yaddnsc_compile_config)
add_sanitizer_flags(yaddnsc_null_driver_fixture)

add_unit_test(driver_loader SOURCE core/driver_loader_test.cpp
    ${PROJECT_SOURCE_DIR}/src/core/driver_loader.cpp
    ${PROJECT_SOURCE_DIR}/src/core/driver_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/core/core_logger.cpp)
set_target_properties(test_driver_loader PROPERTIES ENABLE_EXPORTS ON)
# The loaded driver .so is built with sanitizers — the test binary must be
# too, or ASan aborts with "runtime does not come first" at dlopen time.
add_sanitizer_flags(test_driver_loader)

# macOS + ASan + dlopen plugins: ASan's ODR checker reports the driver's
# vtables as duplicate-registration conflicts (known false positive; the
# plugin is a separate module by design).  Disable ODR detection for this
# test only, keeping the rest of the recommended ASAN_OPTIONS.
set_tests_properties(test_driver_loader PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")
target_compile_definitions(test_driver_loader PRIVATE
    TEST_DRIVER_DIR="${CMAKE_BINARY_DIR}/driver"
    BAD_DRIVER_FIXTURE="$<TARGET_FILE:yaddnsc_bad_driver_fixture>"
    NULL_DRIVER_FIXTURE="$<TARGET_FILE:yaddnsc_null_driver_fixture>")
target_link_libraries(test_driver_loader PRIVATE ${CMAKE_DL_LIBS})
# simple.so must be built before the test runs (dlopen dependency).
add_dependencies(test_driver_loader simple yaddnsc_bad_driver_fixture yaddnsc_null_driver_fixture)

# updater — requires the full DNS + IP-source dependency chain
add_unit_test(updater SOURCE core/updater_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/dispatcher.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser.cpp
    ${PROJECT_SOURCE_DIR}/src/core/updater.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/factory.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/iface.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/http.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/mdns.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/mdns_response.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/iface_util.cpp
    ${PROJECT_SOURCE_DIR}/src/network/socket.cpp
    ${PROJECT_SOURCE_DIR}/src/network/socket_addr.cpp
    ${PROJECT_SOURCE_DIR}/src/network/net_devices.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp)
target_link_libraries(test_updater PRIVATE GTest::gmock glaze::glaze yaddnsc_http_client yaddnsc_net_transport OpenSSL::Crypto)
add_sanitizer_flags(test_updater)

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

# ResolverDispatcher — jthread-based concurrent dispatch
add_unit_test(dispatcher SOURCE dns/dispatcher_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/dispatcher.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/error.cpp)
target_link_libraries(test_dispatcher PRIVATE GTest::gmock OpenSSL::Crypto)

# resolver_registry — factory registry for DNS resolver providers
add_unit_test(resolver_registry SOURCE dns/resolver_registry_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/network/uri.cpp)

# factory — DnsResolverFactory::create (config → ResolverDispatcher)
add_unit_test(factory SOURCE dns/factory_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/factory.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/dispatcher.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/error.cpp)
target_link_libraries(test_factory PRIVATE GTest::gmock OpenSSL::Crypto)

# wire-format builders and queries
add_unit_test(builder SOURCE dns/builder_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp)
target_link_libraries(test_builder PRIVATE OpenSSL::Crypto)

# build_query tests
add_unit_test(mkquery SOURCE dns/mkquery_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp)
target_link_libraries(test_mkquery PRIVATE OpenSSL::Crypto)

# RecordParser — self-contained wire-format parser (no libresolv)
add_unit_test(dns_parser SOURCE dns/parser_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser.cpp)

# DotResolver mock tests — TLS error paths via a mocked net::transport::Stream
add_unit_test(dot_resolver_mock SOURCE dns/dot_resolver_mock_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver/dot.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_dot_resolver_mock PRIVATE yaddnsc_net_transport OpenSSL::SSL OpenSSL::Crypto)
add_sanitizer_flags(test_dot_resolver_mock)

# DohResolver mock tests — TLS + HTTP error paths via a mocked net::transport::Stream
add_unit_test(doh_resolver_mock SOURCE dns/doh_resolver_mock_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver/doh.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_doh_resolver_mock PRIVATE yaddnsc_http_client yaddnsc_net_transport OpenSSL::SSL OpenSSL::Crypto picohttpparser)
add_sanitizer_flags(test_doh_resolver_mock)

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

# socket_mock — MockSocket basic tests
add_unit_test(socket_mock SOURCE network/socket_mock_test.cpp)

# ============================================================================
#  net/  —  net::transport, the redesigned transport layer (no network I/O)
# ============================================================================

add_unit_test(net_transport SOURCE net_transport/net_transport_test.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/detail/socket_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tls_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tcp_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp
    ${PROJECT_SOURCE_DIR}/src/network/inet_address.cpp)
target_link_libraries(test_net_transport PRIVATE OpenSSL::SSL OpenSSL::Crypto)

# ============================================================================
#  http_client/  —  net::http domain: protocol, redirect, form encoding
#  (scripted in-memory Stream, no network I/O)
# ============================================================================

add_unit_test(http_client SOURCE http_client/http_client_test.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/protocol/exchange.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/redirect.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/form_encode.cpp
    ${PROJECT_SOURCE_DIR}/src/network/uri.cpp
    ${PROJECT_SOURCE_DIR}/src/network/inet_address.cpp)
target_link_libraries(test_http_client PRIVATE picohttpparser)

# http_session — Session's persistent-connection policy (retry on connection
# loss, scheme->factory pairing, connect-error mapping) via scripted fakes.
add_unit_test(http_session SOURCE http_client/session_test.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/session.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/stream_factory.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/protocol/exchange.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tcp_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tls_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/detail/socket_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_http_session PRIVATE OpenSSL::SSL OpenSSL::Crypto picohttpparser)

# http_persistent — PersistentClient target/redirect semantics + the shared
# wire-request helpers, driven through a scripted in-memory StreamFactory.
add_unit_test(http_persistent SOURCE http_client/persistent_client_test.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/persistent_client.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/client.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/session.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/stream_factory.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/wire_request.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/redirect.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/protocol/exchange.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tcp_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tls_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/detail/socket_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_http_persistent PRIVATE OpenSSL::SSL OpenSSL::Crypto picohttpparser)

# ============================================================================
#  ip_source/  —  IP-source abstraction (header-only tests only)
# ============================================================================

# mdns_response — datagram parsing/filtering is pure logic; multicast socket
# lifecycle remains covered by the component test.
add_unit_test(mdns_response SOURCE ip_source/mdns_response_test.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/mdns_response.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser.cpp
    ${PROJECT_SOURCE_DIR}/src/network/inet_address.cpp)

# driver_magic — compile-time constant check, header-only
add_unit_test(driver_magic SOURCE ip_source/driver_magic_test.cpp)

# driver_exceptions — ParamParseException hierarchy test, header-only
add_unit_test(driver_exceptions SOURCE ip_source/driver_exceptions_test.cpp)
