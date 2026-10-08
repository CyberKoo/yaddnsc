# ==============================================================================
# Component tests — real I/O in a controlled environment
#
# These tests exercise production code paths that depend on OS resources:
#   - Loopback sockets  (TCP/UDP)
#   - Network interface enumeration
#   - CA certificate path discovery
#   - Local HTTP server + multicast mDNS
#
# They do NOT require external network access or privileged operations.
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
target_link_libraries(test_cert_util_test PRIVATE yaddnsc_net OpenSSL::Crypto)
target_compile_definitions(test_cert_util_test PRIVATE
    YADDNSC_TEST_BINARY="$<TARGET_FILE:test_cert_util_test>"
)

# cert_util env-var hit — SEPARATE binary because discover_ca_bundle() caches
# its result on the first call in the process; the env-var-hit branch can only
# be reached when the first call sees a valid SSL_CERT_FILE.
add_unit_test(cert_util_override SOURCE cert_util_override_test.cpp)
target_link_libraries(test_cert_util_override PRIVATE yaddnsc_net OpenSSL::Crypto)
target_compile_definitions(test_cert_util_override PRIVATE
    YADDNSC_TEST_BINARY="$<TARGET_FILE:test_cert_util_override>"
)

# ============================================================================
#  CLI layer + composition root — pure parser tests, presenter output,
#  diagnostics over fake ports, and Composition::dispatch golden paths
#  (config show/test, driver list/info, dns resolver, interface, info)
#
#  Requires a built driver .so (simple) for the config-test and driver
#  dispatch paths.
# ============================================================================

add_unit_test(cli SOURCE cli_test.cpp)
target_link_libraries(test_cli PRIVATE yaddnsc_prod_all yaddnsc_coro_application yaddnsc_coro GTest::gmock)
target_compile_definitions(test_cli PRIVATE
    TEST_DRIVER_DIR="${CMAKE_BINARY_DIR}/driver"
    YADDNSC_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/template")
# simple.so must be built before the test runs (dlopen dependency).
add_dependencies(test_cli simple)
# macOS + ASan + dlopen plugins: disable the ODR checker for the plugin's
# vtables (same known false positive as test_driver_loader).
set_tests_properties(test_cli PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")

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

# Coroutine HTTP client and DNS subsystem over real loopback I/O: an in-process
# HTTP/1.1 server for plain TCP, and the shared Python dns/doh/dot servers for
# the classic, DoH and DoT paths.
add_unit_test(net_coro_io_component SOURCE net_coro_io_component_test.cpp)
target_link_libraries(test_net_coro_io_component PRIVATE yaddnsc_coro_io OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(test_net_coro_io_component PRIVATE
    TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/test/component"
)

# ============================================================================
#  mDNS — coroutine source over loopback multicast
#
#  A random-UUID hostname is never answered; the test proves the query goes out
#  and the 500 ms response window elapses. Serialized against
#  integration_scenarios, which also answers mDNS on 224.0.0.251:5353.
# ============================================================================

add_unit_test(mdns_coro SOURCE mdns_coro_test.cpp)
target_link_libraries(test_mdns_coro PRIVATE yaddnsc_coro_ip_source yaddnsc_coro)
set_tests_properties(test_mdns_coro PROPERTIES RESOURCE_LOCK "mdns-multicast")
