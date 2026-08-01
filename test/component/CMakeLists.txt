# ==============================================================================
# Component tests — real I/O in a controlled environment
#
# These tests exercise production code paths that depend on OS resources:
#   - Loopback sockets  (TCP/UDP)
#   - Network interface enumeration
#   - CA certificate path discovery
#   - Local HTTP server for HttpIpSource
#
# They do NOT require external network access or privileged operations.
# They are NOT "unit tests" — they validate that individual components
# operate correctly against the real OS environment on loopback.
# ==============================================================================

# The add_unit_test macro is inherited from ../CMakeLists.txt.

# ============================================================================
#  CA certificate path discovery  (filesystem access)
# ============================================================================

add_unit_test(cert_util_test SOURCE cert_util_test.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_cert_util_test PRIVATE OpenSSL::Crypto)

# cert_util env-var hit — SEPARATE binary because discover_ca_bundle() caches
# its result on the first call in the process; the env-var-hit branch can only
# be reached when the first call sees a valid SSL_CERT_FILE.
add_unit_test(cert_util_override SOURCE cert_util_override_test.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_cert_util_override PRIVATE OpenSSL::Crypto)

# ============================================================================
#  CLI layer — parse_and_dispatch + subcommand executors
#  (config show/test, driver list/info, dns resolve/resolver, interface, info)
#
#  Requires a built driver .so (simple) for the config-test executor and the
#  driver subcommands; dns resolve uses a stub resolver (no network).
# ============================================================================

add_unit_test(cli SOURCE cli_test.cpp
    ${PROJECT_SOURCE_DIR}/src/cli/cli.cpp
    ${PROJECT_SOURCE_DIR}/src/cli/config.cpp
    ${PROJECT_SOURCE_DIR}/src/cli/dns.cpp
    ${PROJECT_SOURCE_DIR}/src/cli/driver.cpp
    ${PROJECT_SOURCE_DIR}/src/cli/interface.cpp
    ${PROJECT_SOURCE_DIR}/src/cli/info.cpp
    ${PROJECT_SOURCE_DIR}/src/config/config.cpp
    ${PROJECT_SOURCE_DIR}/src/core/driver_loader.cpp
    ${PROJECT_SOURCE_DIR}/src/core/driver_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/core/core_logger.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/dispatcher.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser/parser_native.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/factory.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/iface_util.cpp
    ${PROJECT_SOURCE_DIR}/src/network/net_devices.cpp)
target_link_libraries(test_cli PRIVATE GTest::gmock glaze::glaze CLI11::CLI11 OpenSSL::Crypto ${CMAKE_DL_LIBS})
target_compile_definitions(test_cli PRIVATE
    YADDNSC_USE_NATIVE_DNS=1
    TEST_DRIVER_DIR="${CMAKE_BINARY_DIR}/driver")
# The loaded driver .so resolves CoreLogger symbols from the host executable.
set_target_properties(test_cli PROPERTIES ENABLE_EXPORTS ON)
# The driver .so is built with sanitizers — the test binary must be too.
add_sanitizer_flags(test_cli)
# simple.so must be built before the test runs (dlopen dependency).
add_dependencies(test_cli simple)
set_tests_properties(test_cli PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")

# ============================================================================
#  Socket RAII wrapper  (loopback UDP/TCP)
# ============================================================================

add_unit_test(socket_test
    ${PROJECT_SOURCE_DIR}/src/network/socket.cpp
    ${PROJECT_SOURCE_DIR}/src/network/socket_addr.cpp)

# ============================================================================
#  Network device enumeration  (loopback interface)
# ============================================================================

add_unit_test(net_devices_test
    ${PROJECT_SOURCE_DIR}/src/network/net_devices.cpp
    ${PROJECT_SOURCE_DIR}/src/network/socket_addr.cpp)

# ============================================================================
#  IP source — InterfaceIpSource  (loopback interface)
# ============================================================================

add_unit_test(ip_source_test
    ${PROJECT_SOURCE_DIR}/src/ip_source/iface.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/iface_util.cpp
    ${PROJECT_SOURCE_DIR}/src/network/net_devices.cpp
    ${PROJECT_SOURCE_DIR}/src/network/socket_addr.cpp)

# ============================================================================
#  HTTP client + HttpIpSource  (local HTTP server)
# ============================================================================

add_unit_test(http_test
    ${PROJECT_SOURCE_DIR}/src/network/http_client.cpp
    ${PROJECT_SOURCE_DIR}/src/ip_source/http.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_http_test PRIVATE httplib::httplib OpenSSL::Crypto)

# ============================================================================
#  mDNS responder + IpSourceFactory  (loopback multicast)
#
# Requires the full DNS + IP-source dependency chain (same as test_updater)
# but tests factory creation and mDNS resolution independently.
# ============================================================================

add_unit_test(factory_mdns SOURCE factory_mdns_test.cpp
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
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp)
target_link_libraries(test_factory_mdns PRIVATE GTest::gmock glaze::glaze httplib::httplib OpenSSL::Crypto)
target_compile_definitions(test_factory_mdns PRIVATE YADDNSC_USE_NATIVE_DNS=1)

# ============================================================================
#  ClassicResolver (native UDP/TCP backend)
#
# Starts a Python DNS server (dns_server.py) on loopback, then queries it
# using the ClassicResolver.  Verifies A/AAAA resolution and error handling.
# ============================================================================

add_unit_test(classic_native_resolver SOURCE classic_native_resolver_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver/classic_native.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/error.cpp
    ${PROJECT_SOURCE_DIR}/src/network/socket.cpp
    ${PROJECT_SOURCE_DIR}/src/network/socket_addr.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp
    ${PROJECT_SOURCE_DIR}/src/network/inet_address.cpp
    ${PROJECT_SOURCE_DIR}/src/network/uri.cpp)
target_compile_definitions(test_classic_native_resolver PRIVATE
    YADDNSC_USE_NATIVE_DNS=1
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)
target_link_libraries(test_classic_native_resolver PRIVATE OpenSSL::Crypto)

# ============================================================================
#  TlsConnection  (loopback TLS echo server via Python + OpenSSL)
#
# Starts a Python TLS echo server with a self-signed cert, then exercises
# TlsConnection::connect/send_all/read_exact/shutdown/is_healthy.
# ============================================================================

add_unit_test(tls_connection SOURCE tls_connection_test.cpp
    ${PROJECT_SOURCE_DIR}/src/network/tls_connection.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_tls_connection PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(test_tls_connection PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)

# ============================================================================
#  TlsStream  (TLS read/write stream adapter)
#
#  Starts a Python TLS echo server on loopback, then exercises
#  TlsStream::send_all / read_some / read_exact and its error
#  mapping (CANCELLED via CancellationSource).
# ============================================================================

add_unit_test(tls_stream SOURCE tls_stream_test.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tls_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/tls_connection.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_tls_stream PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(test_tls_stream PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)

# ============================================================================
#  DotResolver  (DoT — DNS-over-TLS via Python DoT server)
#
# Starts a Python TLS + DNS server on loopback with a self-signed cert,
# then exercises DotResolver::query() for A/AAAA records, error handling,
# and edge cases (timeout, malformed, zero-length responses).
# ============================================================================

add_unit_test(dot_resolver SOURCE dot_resolver_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver/dot.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp
    ${PROJECT_SOURCE_DIR}/src/network/tls_connection.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_dot_resolver PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(test_dot_resolver PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)

# ============================================================================
#  DohResolver  (DoH — DNS-over-HTTPS via Python DoH server)
#
# Starts a Python TLS + HTTP server on loopback with a self-signed cert,
# then exercises DohResolver::query() for A/AAAA records, HTTP error
# handling (404, 500), wrong Content-Type, and timeouts.
# ============================================================================

add_unit_test(doh_resolver SOURCE doh_resolver_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver/doh.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/resolver_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp
    ${PROJECT_SOURCE_DIR}/src/network/tls_connection.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tls_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/http/header_parser.cpp
    ${PROJECT_SOURCE_DIR}/src/http/body_parser.cpp
    ${PROJECT_SOURCE_DIR}/src/http/request.cpp
    ${PROJECT_SOURCE_DIR}/src/http/http.cpp)
target_link_libraries(test_doh_resolver PRIVATE OpenSSL::SSL OpenSSL::Crypto picohttpparser)
target_compile_definitions(test_doh_resolver PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)
