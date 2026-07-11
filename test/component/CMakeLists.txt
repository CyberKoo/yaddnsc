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

add_unit_test(cert_util_test)

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
    ${PROJECT_SOURCE_DIR}/src/ip_source/http.cpp)
target_link_libraries(test_http_test PRIVATE httplib::httplib)

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
    ${PROJECT_SOURCE_DIR}/src/dns/wire/builder.cpp)
target_link_libraries(test_factory_mdns PRIVATE GTest::gmock glaze::glaze httplib::httplib)
target_compile_definitions(test_factory_mdns PRIVATE YADDNSC_USE_NATIVE_DNS=1)
