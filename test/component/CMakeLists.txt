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
# Note: test targets link the production module targets (directly or via
# yaddnsc_prod_all, defined in ../CMakeLists.txt) instead of compiling
# hand-picked production .cpp files.
# ==============================================================================

# The add_unit_test macro is inherited from ../CMakeLists.txt.

# ============================================================================
#  CA certificate path discovery  (filesystem access)
# ============================================================================

add_unit_test(cert_util_test SOURCE cert_util_test.cpp)
target_link_libraries(test_cert_util_test PRIVATE yaddnsc_tls_support OpenSSL::Crypto)
target_compile_definitions(test_cert_util_test PRIVATE
    YADDNSC_TEST_BINARY="$<TARGET_FILE:test_cert_util_test>"
)

# cert_util env-var hit — SEPARATE binary because discover_ca_bundle() caches
# its result on the first call in the process; the env-var-hit branch can only
# be reached when the first call sees a valid SSL_CERT_FILE.
add_unit_test(cert_util_override SOURCE cert_util_override_test.cpp)
target_link_libraries(test_cert_util_override PRIVATE yaddnsc_tls_support OpenSSL::Crypto)
target_compile_definitions(test_cert_util_override PRIVATE
    YADDNSC_TEST_BINARY="$<TARGET_FILE:test_cert_util_override>"
)

# ============================================================================
#  CLI layer + composition root — pure parser tests, presenter output,
#  diagnostics over fake ports, and Composition::dispatch golden paths
#  (config show/test, driver list/info, dns resolver, interface, info)
#
#  Requires a built driver .so (simple) for the config-test and driver
#  dispatch paths; dns resolve is exercised through port mocks (no network).
# ============================================================================

add_unit_test(cli SOURCE cli_test.cpp)
target_link_libraries(test_cli PRIVATE yaddnsc_prod_all GTest::gmock)
target_compile_definitions(test_cli PRIVATE
    TEST_DRIVER_DIR="${CMAKE_BINARY_DIR}/driver"
    YADDNSC_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/template")
# simple.so must be built before the test runs (dlopen dependency).
add_dependencies(test_cli simple)
# macOS + ASan + dlopen plugins: disable the ODR checker for the plugin's
# vtables (same known false positive as test_driver_loader).
set_tests_properties(test_cli PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")

# ============================================================================
#  Socket RAII wrapper  (loopback UDP/TCP)
# ============================================================================

add_unit_test(socket_test SOURCE socket_test.cpp)
target_link_libraries(test_socket_test PRIVATE yaddnsc_network_infrastructure)

# ============================================================================
#  SteadyClock — the Clock adapter over std::chrono::steady_clock
#
#  wait_until() parks until the deadline; wake() must release a parked waiter
#  early (the retry path moves deadlines sooner). Threads + real time.
# ============================================================================

add_unit_test(steady_clock SOURCE steady_clock_test.cpp)
target_link_libraries(test_steady_clock PRIVATE yaddnsc_time_adapter)

# ============================================================================
#  Network device enumeration  (loopback interface)
# ============================================================================

add_unit_test(net_devices_test SOURCE net_devices_test.cpp)
target_link_libraries(test_net_devices_test PRIVATE yaddnsc_network_infrastructure)

# ============================================================================
#  IP source — InterfaceIpSource  (loopback interface)
# ============================================================================

add_unit_test(ip_source_test SOURCE ip_source_test.cpp)
target_link_libraries(test_ip_source_test PRIVATE yaddnsc_ip_source_infrastructure)

# ============================================================================
#  HTTP client + HttpIpSource  (local HTTP server)
# ============================================================================

add_unit_test(http_test SOURCE http_test.cpp)
# UpdateWorkflow tests in this file need the application module on top of the
# IP-source infrastructure (adapter + factory + HttpIpSource).
target_link_libraries(test_http_test PRIVATE yaddnsc_ip_source_infrastructure yaddnsc_application OpenSSL::Crypto)

# ============================================================================
#  mDNS responder + IpSourceFactory  (loopback multicast)
#
# Requires the DNS + IP-source dependency chain but tests factory creation
# and mDNS resolution independently of the update workflow.
# ============================================================================

add_unit_test(factory_mdns SOURCE factory_mdns_test.cpp)
target_link_libraries(test_factory_mdns PRIVATE yaddnsc_ip_source_infrastructure GTest::gmock glaze::glaze OpenSSL::Crypto)
# Both this test and integration_scenarios answer mDNS on 224.0.0.251:5353;
# the sim server catch-alls unknown A queries with 198.51.100.1, so running
# them concurrently makes the UUID-hostname assertions racy. Serialize them.
set_tests_properties(test_factory_mdns PROPERTIES RESOURCE_LOCK "mdns-multicast")

# composition root — dispatch() over every command variant; needs the real
# simple.so so driver diagnostics and environment validation use the actual
# plugin loader.
add_unit_test(composition_bootstrap SOURCE composition_bootstrap_test.cpp)
target_link_libraries(test_composition_bootstrap PRIVATE yaddnsc_prod_all GTest::gmock glaze::glaze OpenSSL::Crypto)
target_compile_definitions(test_composition_bootstrap PRIVATE
    YADDNSC_TEST_DRIVER_DIR="$<TARGET_FILE_DIR:simple>"
    YADDNSC_TEST_DRIVER_FILE="$<TARGET_FILE:simple>"
)

add_dependencies(test_composition_bootstrap simple)
set_tests_properties(test_composition_bootstrap PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")

# mDNS IPv6 / interface-fallback paths. No responder is started, but the
# A-record cases do join 224.0.0.251:5353, so this target must be serialized
# against the other two mDNS group users exactly like test_factory_mdns.
# Every hostname is a random UUID, so the sim server's 198.51.100.1 catch-all
# and any real responder (avahi, systemd-resolved) cannot answer these queries.
add_unit_test(mdns_ipv6 SOURCE mdns_ipv6_test.cpp)
target_link_libraries(test_mdns_ipv6 PRIVATE yaddnsc_ip_source_infrastructure GTest::gmock glaze::glaze OpenSSL::Crypto)
set_tests_properties(test_mdns_ipv6 PROPERTIES RESOURCE_LOCK "mdns-multicast")

# ============================================================================
#  RunLifecycle — the run command's lifecycle object (observable shutdown)
#
#  Pure-fake shutdown-sequence tests (FakeClock + recording executor), plus
#  the full run graph with the real "simple" driver .so through the
#  production dlopen path, fake DNS (fixed wire response) and a blocking fake
#  HttpClient.
#  Locks: stop → no new dispatch → in-flight drained → run() returns →
#  driver module unloaded only afterwards.
# ============================================================================

add_unit_test(run_lifecycle SOURCE run_lifecycle_test.cpp)
target_link_libraries(test_run_lifecycle PRIVATE yaddnsc_prod_all GTest::gmock)
target_compile_definitions(test_run_lifecycle PRIVATE
    TEST_DRIVER_DIR="${CMAKE_BINARY_DIR}/driver")
# simple.so must be built before the test runs (dlopen dependency).
add_dependencies(test_run_lifecycle simple)
# macOS + ASan + dlopen plugins: disable the ODR checker for the plugin's
# vtables (same known false positive as test_driver_loader).
set_tests_properties(test_run_lifecycle PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")

# ============================================================================
#  ClassicResolver (UDP/TCP backend)
#
# Starts a Python DNS server (dns_server.py) on loopback, then queries it
# using the ClassicResolver.  Verifies A/AAAA resolution and error handling.
# ============================================================================

add_unit_test(classic_resolver SOURCE classic_resolver_test.cpp)
target_link_libraries(test_classic_resolver PRIVATE yaddnsc_prod_all)
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

add_unit_test(net_transport_component SOURCE net_transport_test.cpp)
target_link_libraries(test_net_transport_component PRIVATE yaddnsc_network_transport OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(test_net_transport_component PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)

# Successful TCP connect / read / send against a real loopback listener.
# Complements the failure-path coverage in unit/net_transport.
add_unit_test(net_transport_success SOURCE net_transport_success_test.cpp)
target_link_libraries(test_net_transport_success PRIVATE yaddnsc_network_transport OpenSSL::SSL OpenSSL::Crypto)
# TLS WANT_READ / WANT_WRITE direction. A custom BIO forces the opposite
# poll event; the socket is made ready in only one direction.
add_unit_test(tls_direction SOURCE tls_direction_test.cpp)
target_link_libraries(test_tls_direction PRIVATE yaddnsc_network_transport OpenSSL::SSL OpenSSL::Crypto)

# Bootstrap TCP fallback against an in-process DNS server. Complements the
# Python-server case in classic_resolver_test.cpp.
add_unit_test(bootstrap_tcp SOURCE bootstrap_tcp_test.cpp)
target_link_libraries(test_bootstrap_tcp PRIVATE yaddnsc_dns_classic)

# ============================================================================
#  Coroutine transport (src/infrastructure/net/) — TCP echo, the shared Python
#  TLS echo server (with its throwaway self-signed bundle), UDP datagrams and
#  scope-driven timeouts over real loopback I/O.
# ============================================================================

add_unit_test(net_coro_component SOURCE net_coro_component_test.cpp)
target_link_libraries(test_net_coro_component PRIVATE yaddnsc_net OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(test_net_coro_component PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)

# ============================================================================
#  net::http  (Client + Session over an in-process HTTP/1.1 server)
# ============================================================================

add_unit_test(net_http_client SOURCE net_http_client_test.cpp)
target_link_libraries(test_net_http_client PRIVATE yaddnsc_http_infrastructure OpenSSL::SSL OpenSSL::Crypto picohttpparser)

# ============================================================================
#  DotResolver  (DoT — DNS-over-TLS via Python DoT server)
#
# Starts a Python TLS + DNS server on loopback with a self-signed cert,
# then exercises DotResolver::query() for A/AAAA records, error handling,
# and edge cases (timeout, malformed, zero-length responses).
# ============================================================================

add_unit_test(dot_resolver SOURCE dot_resolver_test.cpp)
target_link_libraries(test_dot_resolver PRIVATE yaddnsc_prod_all)
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
target_compile_definitions(test_doh_resolver PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)
