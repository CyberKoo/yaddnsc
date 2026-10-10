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
# The directory layout mirrors src/: domain/, application/, coro/,
# infrastructure/<module>/ and support/ hold the tests of their namesake
# source layer; sdk/ covers the public plugin headers under
# include/yaddnsc/sdk/ and plugin_support/ the crypto helper library.
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
#  domain/  —  pure domain rules (no I/O, no infrastructure)
# ============================================================================

# address_policy — domain::select_address (AAAA link-local/ULA rules)
add_unit_test(address_policy SOURCE domain/address_policy_test.cpp)

# update_decision — domain::decide_update pure decision table
# (implementation in src/domain/update/update_decision.cpp, via test_support)
add_unit_test(update_decision SOURCE domain/update_decision_test.cpp)

# inet_address — uses src/domain/network/inet_address.cpp (via test_support)
add_unit_test(inet_address SOURCE domain/network/inet_address_test.cpp)

# address_family — the AF_INET/AF_INET6 value type
add_unit_test(address_family SOURCE domain/network/address_family_test.cpp)

# DNS domain types — record kinds, query types and the shared error vocabulary
add_unit_test(dns_error    SOURCE domain/dns/dns_error_test.cpp)
add_unit_test(dns_type     SOURCE domain/dns/dns_type_test.cpp)
add_unit_test(record_kind  SOURCE domain/dns/record_kind_test.cpp)

# ============================================================================
#  application/  —  retained use cases and ports
# ============================================================================

# environment_validator — driver-loaded / interface-exists checks against
# fake catalog / interface ports
add_unit_test(application_diagnostics SOURCE application/diagnostics_test.cpp)
target_link_libraries(test_application_diagnostics PRIVATE yaddnsc_application GTest::gmock)

add_unit_test(environment_validator SOURCE application/environment_validator_test.cpp)
target_link_libraries(test_environment_validator PRIVATE yaddnsc_application GTest::gmock glaze::glaze)

# logger_port — the Logger port's default log_explicit() forwards to log();
# the YLOG_* macros format only when is_enabled() (recording double, header-only)
add_unit_test(logger_port SOURCE application/logger_port_test.cpp)

# run_root — the per-subdomain loop coroutines on a ManualClock and the run
# root itself: interval/retry_after/force timing, run_update_cycle decisions, and
# supervisor + SIGINT shutdown, all driven by fake ports.
add_unit_test(run_root SOURCE application/run_root_test.cpp)
target_link_libraries(test_run_root PRIVATE yaddnsc_coro_application yaddnsc_coro yaddnsc_domain)

# ============================================================================
#  cli/  —  argument parsing and output presentation (src/cli/)
# ============================================================================

# parser — subcommand routing, aliases, options, --help/--version
add_unit_test(cli_parser SOURCE cli/parser_test.cpp)
target_link_libraries(test_cli_parser PRIVATE yaddnsc_cli_adapter)

# presenter — stdout/stderr wording and exit codes over hand-built outcomes
add_unit_test(cli_presenter SOURCE cli/presenter_test.cpp)
target_link_libraries(test_cli_presenter PRIVATE yaddnsc_cli_adapter)

# completion — the shell scripts under template/ stay in sync with the
# parser's command/alias set; reads files only, links no production code
add_unit_test(cli_completion SOURCE cli/completion_test.cpp)
target_compile_definitions(test_cli_completion PRIVATE YADDNSC_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/template")

# ============================================================================
#  coro/  —  the coroutine runtime (src/coro/)
# ============================================================================

# Coroutine runtime core — loop, Task, structured scopes, cancellation
# combinators, sleeps, clock reads, AsyncMutex, offload and signals. One
# binary keeps the async runtime tests together:
#   ctest --test-dir <build> -R coro   (or run test_coro directly)
add_unit_test(coro
    SOURCE coro/task_test.cpp
           coro/cancel_test.cpp
           coro/async_mutex_test.cpp
           coro/now_test.cpp
           coro/offload_test.cpp
           coro/signal_test.cpp
           coro/trace_sink_test.cpp
)
target_link_libraries(test_coro PRIVATE yaddnsc_coro)

# Global allocation replacement is isolated from all other runtime tests.
add_unit_test(coro_allocation_failure SOURCE coro/allocation_failure_test.cpp)
target_link_libraries(test_coro_allocation_failure PRIVATE yaddnsc_coro)

# ============================================================================
#  infrastructure/config/  —  configuration parsing, types, and validation
# ============================================================================

add_unit_test(config_parser     SOURCE infrastructure/config/config_parser_test.cpp)
add_unit_test(config_types      SOURCE infrastructure/config/config_types_test.cpp)

# normalizer — raw AppConfig → domain::RuntimeConfig mapping
add_unit_test(config_normalizer SOURCE infrastructure/config/normalizer_test.cpp)
target_link_libraries(test_config_normalizer PRIVATE yaddnsc_config_infrastructure)

# static_validator — collect-all static checks (needs the normalizer for
# validate_and_normalize; uri.cpp/inet_address.cpp come from test_support)
add_unit_test(config_static_validator SOURCE infrastructure/config/static_validator_test.cpp)
target_link_libraries(test_config_static_validator PRIVATE yaddnsc_config_infrastructure)

# config_loader — JSON config file parsing
add_unit_test(config_loader SOURCE infrastructure/config/config_loader_test.cpp)
target_link_libraries(test_config_loader PRIVATE yaddnsc_config_infrastructure)

# ============================================================================
#  infrastructure/plugin/  —  driver loading
# ============================================================================

# driver_loader + driver_catalog — dlopen-based plugin loading
# Fixture: a shared library that is deliberately NOT a yaddnsc driver —
# verifies that auto-discovery skips foreign libraries instead of aborting.
# MODULE produces a .so on both Linux and macOS (SHARED would be .dylib).
add_library(yaddnsc_bad_driver_fixture MODULE infrastructure/plugin/bad_driver_fixture.cpp)

add_unit_test(driver_loader SOURCE infrastructure/plugin/driver_loader_test.cpp)
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

# ============================================================================
#  infrastructure/dns/  —  wire format, parser, resolvers, dispatcher
# ============================================================================

# dns_util — label/name helpers
add_unit_test(dns_util     SOURCE infrastructure/dns/dns_util_test.cpp)

# dns_validator — the response validator (in yaddnsc_dns_classic)
add_unit_test(dns_validator SOURCE infrastructure/dns/dns_validator_test.cpp)
target_link_libraries(test_dns_validator PRIVATE yaddnsc_dns_classic)

# wire-format builders and queries
add_unit_test(builder SOURCE infrastructure/dns/builder_test.cpp)
target_link_libraries(test_builder PRIVATE yaddnsc_dns_classic OpenSSL::Crypto)

# build_query tests
add_unit_test(mkquery SOURCE infrastructure/dns/mkquery_test.cpp)
target_link_libraries(test_mkquery PRIVATE yaddnsc_dns_classic OpenSSL::Crypto)

# RecordParser — self-contained wire-format parser (no libresolv)
add_unit_test(dns_parser SOURCE infrastructure/dns/parser_test.cpp)
add_unit_test(dns_parser_malformed SOURCE infrastructure/dns/parser_malformed_test.cpp)
target_link_libraries(test_dns_parser PRIVATE yaddnsc_dns_classic)
target_link_libraries(test_dns_parser_malformed PRIVATE yaddnsc_dns_classic)

# resolv_conf — /etc/resolv.conf nameserver discovery
add_unit_test(resolv_conf SOURCE infrastructure/dns/resolv_conf_test.cpp)
target_link_libraries(test_resolv_conf PRIVATE yaddnsc_dns_classic)

# Dispatcher strategies (fallback / shuffle / concurrent), retry policy and
# error classification, driven by scripted in-memory resolvers.
add_unit_test(dns_coro_dispatcher SOURCE infrastructure/dns/dispatcher_coro_test.cpp)
target_link_libraries(test_dns_coro_dispatcher PRIVATE yaddnsc_coro_dns)

# Server-address → backend mapping (bare / bracketed IPv6, IPv4, schemas).
# Builds backend objects only — no sockets, no queries.
add_unit_test(dns_factory SOURCE infrastructure/dns/factory_test.cpp)
target_link_libraries(test_dns_factory PRIVATE yaddnsc_coro_dns)

# ============================================================================
#  infrastructure/network/  —  transport, TLS and the socket-address codec
# ============================================================================

# socket_addr — value-type wrapper around sockaddr_storage
add_unit_test(socket_addr SOURCE infrastructure/network/address/socket_addr_test.cpp)
target_link_libraries(test_socket_addr PRIVATE yaddnsc_net)

# fd-wait checkpoint plus TcpStream / TlsStream / UdpSocket over loopback and
# socketpair I/O. The TLS paths here are failure paths only; a real handshake
# and certificate verification live in the component suite.
add_unit_test(net_coro_unit SOURCE infrastructure/network/net_coro_unit_test.cpp)
target_link_libraries(test_net_coro_unit PRIVATE yaddnsc_net OpenSSL::SSL OpenSSL::Crypto)

# ============================================================================
#  infrastructure/uri/  —  URI parsing and encoding
# ============================================================================

# URI parsing — the codec lives in yaddnsc_uri (src/infrastructure/uri/uri.cpp)
add_unit_test(uri_parse SOURCE infrastructure/uri/uri_parse_test.cpp)
target_link_libraries(test_uri_parse PRIVATE yaddnsc_uri)
add_unit_test(uri_query SOURCE infrastructure/uri/uri_query_test.cpp)
target_link_libraries(test_uri_query PRIVATE yaddnsc_uri)
add_unit_test(uri_encode SOURCE infrastructure/uri/uri_encode_test.cpp)
target_link_libraries(test_uri_encode PRIVATE yaddnsc_uri)
add_unit_test(uri_edge   SOURCE infrastructure/uri/uri_edge_test.cpp)
target_link_libraries(test_uri_edge PRIVATE yaddnsc_uri)

# ============================================================================
#  infrastructure/http/  —  coroutine HTTP protocol layer
# ============================================================================

# HTTP protocol layer (src/infrastructure/http/) driven by a scripted
# in-memory stream: framing, 1xx, keep-alive carry-over, the lazy chunked read
# window and the redirect policy.
add_unit_test(net_coro_http SOURCE infrastructure/http/http_coro_test.cpp)
target_link_libraries(test_net_coro_http PRIVATE yaddnsc_http)

# ============================================================================
#  infrastructure/logging/  —  the Logger port implementation on spdlog
# ============================================================================

# spdlog_logger — LogLevel → spdlog::level mapping for every severity, plus the
# source_location (log) and plain-data (log_explicit) overloads.
add_unit_test(spdlog_logger SOURCE infrastructure/logging/spdlog_logger_test.cpp)
target_link_libraries(test_spdlog_logger PRIVATE yaddnsc_logging_adapter spdlog::spdlog)

# async_logging — the async sink: the Logger facade and the direct SPDLOG_*
# call sites deliver through one pipeline, the overrun counter is readable, and
# shutdown drains everything.
add_unit_test(async_logging SOURCE infrastructure/logging/async_logging_test.cpp)
target_link_libraries(test_async_logging PRIVATE yaddnsc_logging_adapter spdlog::spdlog)

# ============================================================================
#  infrastructure/ip_source/  —  IP-source abstraction
# ============================================================================

# mdns_response — datagram parsing/filtering is pure logic; the coroutine
# socket lifecycle is covered by the component test.
add_unit_test(mdns_response SOURCE infrastructure/ip_source/mdns_response_test.cpp)
target_link_libraries(test_mdns_response PRIVATE yaddnsc_ip_source_infrastructure)

# ============================================================================
#  support/  —  algorithms, containers, formatting, platform wrappers
# ============================================================================

add_unit_test(algorithm     SOURCE support/algorithm_test.cpp)
add_unit_test(bytes         SOURCE support/bytes_test.cpp)
add_unit_test(exception     SOURCE support/exception_test.cpp)
add_unit_test(fd            SOURCE support/fd_test.cpp)
add_unit_test(fmt_polyfill  SOURCE support/fmt_polyfill_test.cpp)
add_unit_test(random        SOURCE support/random_test.cpp)
target_link_libraries(test_random PRIVATE OpenSSL::Crypto)
add_unit_test(string_util   SOURCE support/string_util_test.cpp)
add_unit_test(validation    SOURCE support/validation_test.cpp)

# ============================================================================
#  sdk/  —  the public plugin headers (include/yaddnsc/sdk/)
# ============================================================================

add_unit_test(sdk_helpers   SOURCE sdk/sdk_helpers_test.cpp)
add_unit_test(sdk_driver_helpers SOURCE sdk/sdk_driver_helpers_test.cpp)

# driver_magic — the YADDNSC_DRIVER_MAGIC constant in the v1 alpha ABI header
add_unit_test(driver_magic SOURCE sdk/driver_magic_test.cpp)

# ============================================================================
#  plugin_support/  —  the crypto helper library (plugin_support/crypto/)
# ============================================================================

# signing — cryptographic primitives (SHA, HMAC, Base64, hex) from the
# plugin crypto library (also linked into signing-enabled drivers).
add_unit_test(signing SOURCE plugin_support/signing_test.cpp)
target_include_directories(test_signing PRIVATE ${PROJECT_SOURCE_DIR}/plugin_support/crypto)
target_link_libraries(test_signing PRIVATE yaddnsc_plugin_support_crypto OpenSSL::Crypto)
