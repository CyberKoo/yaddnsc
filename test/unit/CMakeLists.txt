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
#
# Note: test targets link the production module targets (directly or via
# yaddnsc_prod_all, defined in ../CMakeLists.txt) instead of compiling
# hand-picked production .cpp files.
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
add_unit_test(sdk_helpers   SOURCE util/sdk_helpers_test.cpp)
add_unit_test(mixin         SOURCE util/mixin_test.cpp)
add_unit_test(random        SOURCE util/random_test.cpp)
target_link_libraries(test_random PRIVATE OpenSSL::Crypto)
add_unit_test(retry_util    SOURCE util/retry_util_test.cpp)
add_unit_test(string_util   SOURCE util/string_util_test.cpp)
add_unit_test(validation    SOURCE util/validation_test.cpp)

# signing — cryptographic primitives (SHA, HMAC, Base64, hex) from the
# plugin crypto library (also linked into signing-enabled drivers).
add_unit_test(signing SOURCE util/signing_test.cpp)
target_include_directories(test_signing PRIVATE ${PROJECT_SOURCE_DIR}/plugin_crypto)
target_link_libraries(test_signing PRIVATE yaddnsc_plugin_crypto OpenSSL::Crypto)

# ============================================================================
#  common/  —  cross-module base types (address_family, exceptions, etc.)
# ============================================================================

add_unit_test(address_family SOURCE common/address_family_test.cpp)
add_unit_test(exception      SOURCE common/exception_test.cpp)
add_unit_test(resolver_base  SOURCE common/resolver_base_test.cpp)

# ============================================================================
#  domain/  —  pure domain rules (no I/O, no infrastructure)
# ============================================================================

# address_policy — domain::select_address (AAAA link-local/ULA rules)
add_unit_test(address_policy SOURCE domain/address_policy_test.cpp)

# update_decision — domain::decide_update pure decision table (header-only)
add_unit_test(update_decision SOURCE domain/update_decision_test.cpp)

# schedule_queue — domain::ScheduleQueue over injected (fake) time
add_unit_test(schedule_queue SOURCE domain/schedule_queue_test.cpp)
target_link_libraries(test_schedule_queue PRIVATE yaddnsc_domain yaddnsc_config_infrastructure glaze::glaze)

# ============================================================================
#  application/  —  update workflow, scheduler runner, task executor
# ============================================================================

# update_workflow — the update use case against port mocks
add_unit_test(update_workflow SOURCE application/update_workflow_test.cpp)
target_link_libraries(test_update_workflow PRIVATE yaddnsc_application yaddnsc_config_infrastructure GTest::gmock glaze::glaze)

# scheduler_runner — the scheduling loop over FakeClock + recording executor
add_unit_test(scheduler_runner SOURCE application/scheduler_runner_test.cpp)
target_link_libraries(test_scheduler_runner PRIVATE yaddnsc_application yaddnsc_config_infrastructure GTest::gmock glaze::glaze)

# pool_task_executor — pool-backed executor: drain, reject-after-shutdown,
# same-module concurrency
add_unit_test(pool_task_executor SOURCE application/pool_task_executor_test.cpp)
target_link_libraries(test_pool_task_executor PRIVATE yaddnsc_application yaddnsc_config_infrastructure GTest::gmock glaze::glaze)

# environment_validator — driver-loaded / interface-exists checks against
# fake catalog / interface ports
add_unit_test(environment_validator SOURCE application/environment_validator_test.cpp)
target_link_libraries(test_environment_validator PRIVATE yaddnsc_application GTest::gmock glaze::glaze)

# logger_port — the Logger port's default log_explicit() forwards to log();
# the YLOG_* macros format only when is_enabled() (recording double, header-only)
add_unit_test(logger_port SOURCE application/logger_port_test.cpp)

# ============================================================================
#  config/  —  configuration parsing, types, and validation
# ============================================================================

add_unit_test(config_parser     SOURCE config/config_parser_test.cpp)
add_unit_test(config_types      SOURCE config/config_types_test.cpp)

# normalizer — raw AppConfig → domain::RuntimeConfig mapping
add_unit_test(config_normalizer SOURCE config/normalizer_test.cpp)
target_link_libraries(test_config_normalizer PRIVATE yaddnsc_config_infrastructure)

# static_validator — collect-all static checks (needs the normalizer for
# validate_and_normalize; uri.cpp/inet_address.cpp come from test_support)
add_unit_test(config_static_validator SOURCE config/static_validator_test.cpp)
target_link_libraries(test_config_static_validator PRIVATE yaddnsc_config_infrastructure)

# config_loader — JSON config file parsing
add_unit_test(config_loader SOURCE config/config_loader_test.cpp)
target_link_libraries(test_config_loader PRIVATE yaddnsc_config_infrastructure)

# ============================================================================
#  core/  —  signal watcher, driver loading, driver gateway
# ============================================================================

# signal_watcher — install/construct/destroy lifecycle (signals are blocked
# process-wide by install(), so this binary only runs these tests).
add_unit_test(signal_watcher SOURCE core/signal_watcher_test.cpp)
target_link_libraries(test_signal_watcher PRIVATE yaddnsc_process_adapter)

# driver_loader + driver_catalog — dlopen-based plugin loading
# Fixture: a shared library that is deliberately NOT a yaddnsc driver —
# verifies that auto-discovery skips foreign libraries instead of aborting.
# MODULE produces a .so on both Linux and macOS (SHARED would be .dylib).
add_library(yaddnsc_bad_driver_fixture MODULE core/bad_driver_fixture.cpp)

add_unit_test(driver_loader SOURCE core/driver_loader_test.cpp)
target_link_libraries(test_driver_loader PRIVATE yaddnsc_prod_all)

# macOS + ASan + dlopen plugins: ASan's ODR checker reports the driver's
# vtables as duplicate-registration conflicts (known false positive; the
# plugin is a separate module by design).  Disable ODR detection for this
# test only, keeping the rest of the recommended ASAN_OPTIONS.
set_tests_properties(test_driver_loader PROPERTIES
    ENVIRONMENT "ASAN_OPTIONS=detect_odr_violation=0:detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2")
target_compile_definitions(test_driver_loader PRIVATE
    TEST_DRIVER_DIR="${CMAKE_BINARY_DIR}/driver"
    BAD_DRIVER_FIXTURE="$<TARGET_FILE:yaddnsc_bad_driver_fixture>")
target_link_libraries(test_driver_loader PRIVATE ${CMAKE_DL_LIBS})
# simple.so must be built before the test runs (dlopen dependency).
add_dependencies(test_driver_loader simple yaddnsc_bad_driver_fixture)

# updater — superseded by application/update_workflow (see
# application/update_workflow_test.cpp).

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
add_unit_test(dispatcher SOURCE dns/dispatcher_test.cpp)
target_link_libraries(test_dispatcher PRIVATE yaddnsc_dns_infrastructure GTest::gmock OpenSSL::Crypto)

# resolver_catalog — instance-level schema→factory catalog (replaces the
# global DnsResolverRegistry tests; construction-only for the built-ins,
# no network I/O)
add_unit_test(resolver_catalog SOURCE dns/resolver_catalog_test.cpp)
target_link_libraries(test_resolver_catalog PRIVATE yaddnsc_dns_infrastructure)

# factory — DnsResolverFactory::create (config → ResolverDispatcher) with an
# injected stub catalog
add_unit_test(factory SOURCE dns/factory_test.cpp)
target_link_libraries(test_factory PRIVATE yaddnsc_prod_all)

# wire-format builders and queries
add_unit_test(builder SOURCE dns/builder_test.cpp)
target_link_libraries(test_builder PRIVATE yaddnsc_dns_infrastructure OpenSSL::Crypto)

# build_query tests
add_unit_test(mkquery SOURCE dns/mkquery_test.cpp)
target_link_libraries(test_mkquery PRIVATE yaddnsc_dns_infrastructure OpenSSL::Crypto)

# RecordParser — self-contained wire-format parser (no libresolv)
add_unit_test(dns_parser SOURCE dns/parser_test.cpp)
target_link_libraries(test_dns_parser PRIVATE yaddnsc_dns_infrastructure)

# bootstrap — hostname resolution via bootstrap DNS servers (no-network paths:
# empty server list, expired deadline, pre-cancelled token)
add_unit_test(dns_bootstrap SOURCE dns/bootstrap_test.cpp)
target_link_libraries(test_dns_bootstrap PRIVATE yaddnsc_dns_classic)

# resolv_conf — /etc/resolv.conf nameserver discovery
add_unit_test(resolv_conf SOURCE dns/resolv_conf_test.cpp)
target_link_libraries(test_resolv_conf PRIVATE yaddnsc_dns_classic)

# DotResolver mock tests — TLS error paths via a mocked net::transport::Stream
add_unit_test(dot_resolver_mock SOURCE dns/dot_resolver_mock_test.cpp)
target_link_libraries(test_dot_resolver_mock PRIVATE yaddnsc_prod_all)

# DohResolver mock tests — TLS + HTTP error paths via a mocked net::transport::Stream
add_unit_test(doh_resolver_mock SOURCE dns/doh_resolver_mock_test.cpp)
target_link_libraries(test_doh_resolver_mock PRIVATE yaddnsc_prod_all)

# ============================================================================
#  network/  —  network address/path/URI parsing (pure logic, no I/O)
# ============================================================================

# inet_address — uses inet_address.cpp (already in test_support)
add_unit_test(inet_address SOURCE network/inet_address_test.cpp)

# socket_addr — value-type wrapper around sockaddr_storage
add_unit_test(socket_addr SOURCE network/socket_addr_test.cpp)
target_link_libraries(test_socket_addr PRIVATE yaddnsc_network_infrastructure)

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

add_unit_test(net_transport SOURCE net_transport/net_transport_test.cpp)
target_link_libraries(test_net_transport PRIVATE yaddnsc_network_transport OpenSSL::SSL OpenSSL::Crypto)

# ============================================================================
#  logging/  —  the Logger port implementation on top of spdlog
# ============================================================================

# spdlog_logger — LogLevel → spdlog::level mapping for every severity, plus the
# source_location (log) and plain-data (log_explicit) overloads.
add_unit_test(spdlog_logger SOURCE logging/spdlog_logger_test.cpp)
target_link_libraries(test_spdlog_logger PRIVATE yaddnsc_logging_adapter spdlog::spdlog)

# ============================================================================
#  http_client/  —  net::http domain: protocol, redirect
#  (scripted in-memory Stream, no network I/O)
# ============================================================================

add_unit_test(http_client SOURCE http_client/http_client_test.cpp)
target_link_libraries(test_http_client PRIVATE yaddnsc_http_infrastructure picohttpparser)

# http_session — Session's persistent-connection policy (retry on connection
# loss, scheme->factory pairing, connect-error mapping) via scripted fakes.
add_unit_test(http_session SOURCE http_client/session_test.cpp)
target_link_libraries(test_http_session PRIVATE yaddnsc_http_infrastructure OpenSSL::SSL OpenSSL::Crypto picohttpparser)

# http_persistent — PersistentClient target/redirect semantics + the shared
# wire-request helpers, driven through a scripted in-memory StreamFactory.
add_unit_test(http_persistent SOURCE http_client/persistent_client_test.cpp)
target_link_libraries(test_http_persistent PRIVATE yaddnsc_http_infrastructure OpenSSL::SSL OpenSSL::Crypto picohttpparser)

# http_internal_headers — host-internal HTTP/transport headers remain
# self-contained under the src/ include root.
add_unit_test(http_internal_headers SOURCE http_client/internal_headers_test.cpp)

# ============================================================================
#  ip_source/  —  IP-source abstraction
# ============================================================================

# ip_source_adapter — IpSourcePort contract over the legacy throwing stack
add_unit_test(ip_source_adapter SOURCE ip_source/adapter_test.cpp)
target_link_libraries(test_ip_source_adapter PRIVATE yaddnsc_ip_source_infrastructure GTest::gmock)

# mdns_response — datagram parsing/filtering is pure logic; multicast socket
# lifecycle remains covered by the component test.
add_unit_test(mdns_response SOURCE ip_source/mdns_response_test.cpp)
target_link_libraries(test_mdns_response PRIVATE yaddnsc_ip_source_infrastructure)

# driver_magic — the YADDNSC_DRIVER_MAGIC constant in the v1 alpha ABI header
add_unit_test(driver_magic SOURCE ip_source/driver_magic_test.cpp)
