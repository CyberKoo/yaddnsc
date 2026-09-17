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
#
# Phase 2 note: migrated targets link the production OBJECT libraries via
# yaddnsc_prod_all (defined in ../CMakeLists.txt) instead of compiling
# hand-picked production .cpp files.
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

add_unit_test(cli SOURCE cli_test.cpp)
target_link_libraries(test_cli PRIVATE yaddnsc_prod_all GTest::gmock)
target_compile_definitions(test_cli PRIVATE
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
    ${PROJECT_SOURCE_DIR}/src/ip_source/http.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_http_test PRIVATE yaddnsc_http_client yaddnsc_net_transport OpenSSL::Crypto)
add_sanitizer_flags(test_http_test)

# ============================================================================
#  mDNS responder + IpSourceFactory  (loopback multicast)
#
# Requires the DNS + IP-source dependency chain but tests factory creation
# and mDNS resolution independently of the update workflow.
# ============================================================================

add_unit_test(factory_mdns SOURCE factory_mdns_test.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/dispatcher.cpp
    ${PROJECT_SOURCE_DIR}/src/dns/parser.cpp
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
target_link_libraries(test_factory_mdns PRIVATE GTest::gmock glaze::glaze yaddnsc_http_client yaddnsc_net_transport OpenSSL::Crypto)
# Object libs carry sanitizer instrumentation — the test binary must link it too.
add_sanitizer_flags(test_factory_mdns)
# Both this test and integration_scenarios answer mDNS on 224.0.0.251:5353;
# the sim server catch-alls unknown A queries with 198.51.100.1, so running
# them concurrently makes the UUID-hostname assertions racy. Serialize them.
set_tests_properties(test_factory_mdns PROPERTIES RESOURCE_LOCK "mdns-multicast")

# ============================================================================
#  Manager lifecycle  (observable shutdown sequence; Phase 0 §0.5)
#
#  Real Manager + real "simple" driver .so through the production dlopen path,
#  with fake DNS (fixed wire response) and a blocking fake HttpClient.
#  Locks: stop → no new dispatch → in-flight drained → run() returns →
#  driver module unloaded only afterwards.
# ============================================================================

add_unit_test(manager_lifecycle SOURCE manager_lifecycle_test.cpp)
target_link_libraries(test_manager_lifecycle PRIVATE yaddnsc_prod_all GTest::gmock)
target_compile_definitions(test_manager_lifecycle PRIVATE
    TEST_DRIVER_DIR="${CMAKE_BINARY_DIR}/driver")
# The loaded driver .so resolves CoreLogger symbols from the host executable.
set_target_properties(test_manager_lifecycle PROPERTIES ENABLE_EXPORTS ON)
# The driver .so is built with sanitizers — the test binary must be too.
add_sanitizer_flags(test_manager_lifecycle)
# simple.so must be built before the test runs (dlopen dependency).
add_dependencies(test_manager_lifecycle simple)
set_tests_properties(test_manager_lifecycle PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")

# ============================================================================
#  ClassicResolver (UDP/TCP backend)
#
# Starts a Python DNS server (dns_server.py) on loopback, then queries it
# using the ClassicResolver.  Verifies A/AAAA resolution and error handling.
# ============================================================================

add_unit_test(classic_resolver SOURCE classic_resolver_test.cpp)
target_link_libraries(test_classic_resolver PRIVATE yaddnsc_prod_all)
add_sanitizer_flags(test_classic_resolver)
target_compile_definitions(test_classic_resolver PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)

# ============================================================================
#  net::transport  (new transport layer — TcpStream + TlsStream)
#
# TcpStream against an in-process loopback echo server; TlsStream against
# the Python TLS echo server. Covers roundtrips, idempotent
# ensure_connected(), verification modes, and cross-thread cancellation.
# ============================================================================

add_unit_test(net_transport_component SOURCE net_transport_test.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/detail/socket_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tls_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tcp_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp
    ${PROJECT_SOURCE_DIR}/src/network/inet_address.cpp)
target_link_libraries(test_net_transport_component PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(test_net_transport_component PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)

# ============================================================================
#  net::http  (Client + Session over an in-process HTTP/1.1 server)
# ============================================================================

add_unit_test(net_http_client SOURCE net_http_client_test.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/client.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/persistent_client.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/session.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/redirect.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/stream_factory.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/wire_request.cpp
    ${PROJECT_SOURCE_DIR}/src/http_client/protocol/exchange.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/detail/socket_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tcp_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/transport/tls_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/network/uri.cpp
    ${PROJECT_SOURCE_DIR}/src/network/inet_address.cpp
    ${PROJECT_SOURCE_DIR}/src/util/cert_util.cpp)
target_link_libraries(test_net_http_client PRIVATE OpenSSL::SSL OpenSSL::Crypto picohttpparser)

# ============================================================================
#  DotResolver  (DoT — DNS-over-TLS via Python DoT server)
#
# Starts a Python TLS + DNS server on loopback with a self-signed cert,
# then exercises DotResolver::query() for A/AAAA records, error handling,
# and edge cases (timeout, malformed, zero-length responses).
# ============================================================================

add_unit_test(dot_resolver SOURCE dot_resolver_test.cpp)
target_link_libraries(test_dot_resolver PRIVATE yaddnsc_prod_all)
add_sanitizer_flags(test_dot_resolver)
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

add_unit_test(doh_resolver SOURCE doh_resolver_test.cpp)
target_link_libraries(test_doh_resolver PRIVATE yaddnsc_prod_all)
add_sanitizer_flags(test_doh_resolver)
target_compile_definitions(test_doh_resolver PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)
