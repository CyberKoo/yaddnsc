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

# main_loop — the per-subdomain task coroutines on a ManualClock and the main
# loop itself: interval/retry_after/force timing, update_task decisions, and
# supervisor + SIGINT shutdown, all driven by fake ports.
add_unit_test(main_loop SOURCE application/main_loop_test.cpp)
target_link_libraries(test_main_loop PRIVATE yaddnsc_coro_application yaddnsc_coro yaddnsc_domain)

# diagnostics_paths — list_drivers / list_interfaces enumeration shapes and
# port failures, the dns_resolve record-kind table, the validation sweep's
# zero/multi-domain and dedup branches
add_unit_test(diagnostics_paths SOURCE application/diagnostics_paths_test.cpp)
target_link_libraries(test_diagnostics_paths PRIVATE yaddnsc_coro_application yaddnsc_application yaddnsc_coro yaddnsc_domain)

# update_task_paths — a single update cycle's port-failure arms and the three
# exception arms
add_unit_test(update_task_paths SOURCE application/update_task_paths_test.cpp)
target_link_libraries(test_update_task_paths PRIVATE yaddnsc_coro_application yaddnsc_coro yaddnsc_domain)

# subdomain_task_paths — non-positive interval clamping and the force-due
# equality boundary, on a ManualClock
add_unit_test(subdomain_task_paths SOURCE application/subdomain_task_paths_test.cpp)
target_link_libraries(test_subdomain_task_paths PRIVATE yaddnsc_coro_application yaddnsc_coro yaddnsc_domain)

# ============================================================================
#  coro/  —  process stop-signal ownership
# ============================================================================

# stop_signals — one process handler and a latched pipe for startup, runtime
# and drain; ownership restores handlers between tests/runs.
add_unit_test(stop_signals SOURCE coro/stop_signals_test.cpp)
target_link_libraries(test_stop_signals PRIVATE yaddnsc_coro)
# Inject handler-installation failures without a production test hook.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(test_stop_signals PRIVATE "LINKER:--wrap=sigaction")
    target_compile_definitions(test_stop_signals PRIVATE YADDNSC_TEST_WRAP_SIGACTION=1)
endif()

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
# combinators, sleeps, clock reads, AsyncMutex, offload and process stop handling. One
# binary keeps the async runtime tests together:
#   ctest --test-dir <build> -R coro   (or run test_coro directly)
add_unit_test(coro
    SOURCE coro/task_test.cpp
           coro/cancel_test.cpp
           coro/cancel_tree_test.cpp
           coro/async_mutex_test.cpp
           coro/now_test.cpp
           coro/offload_test.cpp
           coro/trace_sink_test.cpp
           coro/loop_test.cpp
           coro/context_contract_test.cpp
           coro/task_group_contract_test.cpp
           coro/scope_group_paths_test.cpp
           coro/host_test.cpp
           coro/notification_test.cpp
)
target_link_libraries(test_coro PRIVATE yaddnsc_coro)

# GNU linker wrapping stays in an isolated Linux-only fault-injection binary.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_unit_test(coro_syscall_failure SOURCE coro/syscall_failure_test.cpp)
    target_link_libraries(test_coro_syscall_failure PRIVATE yaddnsc_coro)
    target_link_options(test_coro_syscall_failure PRIVATE "LINKER:--wrap=write" "LINKER:--wrap=poll")
endif()

# Global allocation replacement is isolated from all other runtime tests.
add_unit_test(coro_allocation_failure SOURCE coro/allocation_failure_test.cpp)
target_link_libraries(test_coro_allocation_failure PRIVATE yaddnsc_coro test_allocation_fault)

# ============================================================================
#  composition/  —  the composition root (src/composition/)
# ============================================================================

# composition_assembly — log-level mapping, the fill_bootstrap_servers early
# return, gateway / HTTP option construction and the config-error formatter
add_unit_test(composition_assembly SOURCE composition/assembly_test.cpp)
target_link_libraries(test_composition_assembly PRIVATE yaddnsc_prod_all)

# run_command — the CLI token → severity mapping and the update-policy budget
# invariants execute_command wires in (the command itself needs real I/O and
# stays in test/component/cli_test.cpp)
add_unit_test(run_command SOURCE composition/run_command_test.cpp)
target_link_libraries(test_run_command PRIVATE yaddnsc_prod_all)

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

# diagnostics_renderer — the config diagnostics wording: every named ErrorKind
# sentence and the prose-kind fallback, asserted through render()
add_unit_test(diagnostics_renderer SOURCE infrastructure/config/diagnostics_renderer_test.cpp)
target_link_libraries(test_diagnostics_renderer PRIVATE yaddnsc_config_infrastructure)

# diagnostics_error_adapter — glz::error_code → ErrorKind mapping incl. the
# OTHER fallback keeping the Glaze code name
add_unit_test(diagnostics_error_adapter SOURCE infrastructure/config/diagnostics_error_adapter_test.cpp)
target_link_libraries(test_diagnostics_error_adapter PRIVATE yaddnsc_config_infrastructure)

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

# host_services — the host-side ABI callbacks: log-level / HTTP-method mapping,
# request-view validation and the exception arms, driven through a closed Bridge
add_unit_test(host_services SOURCE infrastructure/plugin/host_services_test.cpp)
target_link_libraries(test_host_services PRIVATE yaddnsc_prod_all test_allocation_fault)

# bridge_branch — the Bridge ingress closure arms: closed-group start_cycle,
# spawn / dispatch allocation failures, and the cancel() no-op arms
add_unit_test(bridge_branch SOURCE infrastructure/plugin/bridge_branch_test.cpp)
target_link_libraries(test_bridge_branch PRIVATE yaddnsc_prod_all test_allocation_fault)

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

# wire framing — the TCP length-prefix codec and its size bounds
add_unit_test(dns_framing SOURCE infrastructure/dns/framing_test.cpp)
target_link_libraries(test_dns_framing PRIVATE yaddnsc_dns_classic)

# resolv_conf — /etc/resolv.conf nameserver discovery
add_unit_test(resolv_conf SOURCE infrastructure/dns/resolv_conf_test.cpp)
target_link_libraries(test_resolv_conf PRIVATE yaddnsc_dns_classic)

# Dispatcher strategies (fallback / shuffle / concurrent), retry policy and
# error classification, driven by scripted in-memory resolvers.
add_unit_test(dns_coro_dispatcher SOURCE infrastructure/dns/dispatcher_coro_test.cpp)
target_link_libraries(test_dns_coro_dispatcher PRIVATE yaddnsc_coro_dns)

# dns_coro_dispatcher_paths — the concurrency guard, the negative-answer
# summary, the single-candidate recovery round and the race that ends without
# a diagnosis, on scripted in-memory resolvers
add_unit_test(dns_coro_dispatcher_paths SOURCE infrastructure/dns/dispatcher_paths_test.cpp)
target_link_libraries(test_dns_coro_dispatcher_paths PRIVATE yaddnsc_coro_dns)

# dns_coro_bootstrap_paths — the bootstrap server-entry policy and one server's
# response, driven by an in-memory replacement for dns::detail::query_classic
# (see the file header). Links ONLY yaddnsc_coro_dns: anything else defining
# query_classic would collide with the stand-in at link time.
add_unit_test(dns_coro_bootstrap_paths SOURCE infrastructure/dns/bootstrap_paths_test.cpp)
target_link_libraries(test_dns_coro_bootstrap_paths PRIVATE yaddnsc_coro_dns)

# Server-address → backend mapping (bare / bracketed IPv6, IPv4, schemas).
# Builds backend objects only — no sockets, no queries.
add_unit_test(dns_factory SOURCE infrastructure/dns/factory_test.cpp)
target_link_libraries(test_dns_factory PRIVATE yaddnsc_coro_dns yaddnsc_config_infrastructure)

# DoT / DoH budgets and error mapping, driven by an injected in-memory
# StreamFactory on a manual clock: the DoT connect budget, the catch-all
# UNKNOWN mapping of both resolvers, the DoH per-address connect timeout and
# the bootstrap server policy. No sockets.
add_unit_test(dns_coro_resolver SOURCE infrastructure/dns/resolver_coro_test.cpp)
target_link_libraries(test_dns_coro_resolver PRIVATE yaddnsc_coro_dns yaddnsc_coro)

# dns_coro_doh_paths — DoH close() idempotence / keep-alive release and the
# INVALID_URL → CONFIG error mapping, on an injected in-memory StreamFactory
add_unit_test(dns_coro_doh_paths SOURCE infrastructure/dns/doh_paths_test.cpp)
target_link_libraries(test_dns_coro_doh_paths PRIVATE yaddnsc_coro_dns yaddnsc_coro)

# dns_coro_dot_paths — the DoT endpoint contract enforced at construction, the
# SNI default, and close() on a resolver that owns a connection
add_unit_test(dns_coro_dot_paths SOURCE infrastructure/dns/dot_paths_test.cpp)
target_link_libraries(test_dns_coro_dot_paths PRIVATE yaddnsc_coro_dns yaddnsc_coro)

# dns_coro_exchange_socket_failure — the classic UDP exchange's
# connection_error() arm, reached by replacing socket(2); own executable
# because the replacement is process-wide
add_unit_test(dns_coro_exchange_socket_failure SOURCE infrastructure/dns/exchange_socket_failure_test.cpp)
target_link_libraries(test_dns_coro_exchange_socket_failure PRIVATE yaddnsc_coro_dns yaddnsc_coro)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_dns_coro_exchange_socket_failure PRIVATE YADDNSC_TEST_WRAP_SOCKET=1)
    target_link_options(test_dns_coro_exchange_socket_failure PRIVATE "LINKER:--wrap=socket")
endif()

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

# net_socket_paths — socket_ops EBADF / empty-interface / loopback success
# paths and the shuffle-safe CA-bundle discovery invariants. The loopback
# interface name comes from the ipsource getifaddrs snapshot ("lo" vs "lo0").
add_unit_test(net_socket_paths SOURCE infrastructure/network/tls_socket_paths_test.cpp)
target_link_libraries(test_net_socket_paths PRIVATE yaddnsc_net yaddnsc_ip_source_infrastructure OpenSSL::Crypto)

# net_stream_paths — TcpStream/TlsStream paths over loopback that an echo
# round-trip never reaches: SNI/ALPN session construction and the health probe
# (EOF, idle, peer reset). Failure paths only; no real handshake.
add_unit_test(net_stream_paths SOURCE infrastructure/network/net_stream_paths_test.cpp)
target_link_libraries(test_net_stream_paths PRIVATE yaddnsc_net OpenSSL::SSL OpenSSL::Crypto)

# ============================================================================
#  infrastructure/uri/  —  URI parsing and encoding
# ============================================================================

# URI parsing — the codec lives in yaddnsc_uri (src/infrastructure/uri/uri.cpp)
add_unit_test(uri_parse SOURCE infrastructure/uri/uri_parse_test.cpp)
target_link_libraries(test_uri_parse PRIVATE yaddnsc_uri)
add_unit_test(uri_resolve SOURCE infrastructure/uri/uri_resolve_test.cpp)
target_link_libraries(test_uri_resolve PRIVATE yaddnsc_uri)
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
# window and the redirect policy, plus wire-request construction/validation
# and the exchange request/response grammar guards, and the client/session/
# persistent-client paths (redirect loop, replay, keep-alive budget, queue
# timeout, origin/target resolution), and the RFC 9110 field-syntax predicates
# (tchar set, field-value character rules) the builder and parser share.
add_unit_test(net_coro_http
    SOURCE infrastructure/http/http_coro_test.cpp
           infrastructure/http/wire_request_test.cpp
           infrastructure/http/exchange_paths_test.cpp
           infrastructure/http/client_redirect_test.cpp
           infrastructure/http/session_paths_test.cpp
           infrastructure/http/field_chars_test.cpp
)
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

# level_enabled — the header-only severity guard: it follows the active spdlog
# level and reports disabled once shutdown left no default logger. Header-only,
# so it needs no production module beyond the spdlog the test config provides.
add_unit_test(level_enabled SOURCE infrastructure/logging/level_enabled_test.cpp)

# ============================================================================
#  infrastructure/ip_source/  —  IP-source abstraction
# ============================================================================

# mdns_response — datagram parsing/filtering is pure logic; the coroutine
# socket lifecycle is covered by the component test.
add_unit_test(mdns_response SOURCE infrastructure/ip_source/mdns_response_test.cpp)
target_link_libraries(test_mdns_response PRIVATE yaddnsc_ip_source_infrastructure)

# mdns_query — the socket setup (interface metadata, multicast options, group
# join) reached through MdnsIpSource::resolve(), with setsockopt fault
# injection; the rejected group join ends every lookup before it is sent.
add_unit_test(mdns_query SOURCE infrastructure/ip_source/mdns_query_test.cpp)
target_link_libraries(test_mdns_query PRIVATE yaddnsc_coro_ip_source yaddnsc_coro
                                              yaddnsc_ip_source_infrastructure yaddnsc_net)
# Fault injection for setsockopt; the cases compile only under this macro.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_mdns_query PRIVATE YADDNSC_TEST_WRAP_SETSOCKOPT=1)
    target_link_options(test_mdns_query PRIVATE "LINKER:--wrap=setsockopt")
endif()

# ip_source_paths — the interface, HTTP and adapter sources: the record-kind →
# address-family mapping, the family/interface overrides applied at HttpIpSource
# construction, and the HTTP response grammar, all on in-memory injection points
# (net::StreamFactory + hostname resolver) and a getifaddrs snapshot. No sockets.
add_unit_test(ip_source_paths SOURCE infrastructure/ip_source/ip_source_paths_test.cpp)
target_link_libraries(test_ip_source_paths PRIVATE yaddnsc_coro_ip_source yaddnsc_coro
                                                   yaddnsc_ip_source_infrastructure yaddnsc_net)

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
# RAND_bytes is replaced only in this executable to exercise fail-closed paths.
add_unit_test(random_failure SOURCE support/random_failure_test.cpp)
target_link_libraries(test_random_failure PRIVATE yaddnsc_coro_dns)
add_unit_test(string_util   SOURCE support/string_util_test.cpp)
add_unit_test(validation    SOURCE support/validation_test.cpp)

# ============================================================================
#  sdk/  —  the public plugin headers (include/yaddnsc/sdk/)
# ============================================================================

add_unit_test(sdk_helpers   SOURCE sdk/sdk_helpers_test.cpp)
add_unit_test(sdk_driver_helpers SOURCE sdk/sdk_driver_helpers_test.cpp)

# driver_magic — the YADDNSC_DRIVER_MAGIC constant in the v1 ABI header
add_unit_test(driver_magic SOURCE sdk/driver_magic_test.cpp)

# ============================================================================
#  plugin_support/ — tests for SDK signing helpers
# ============================================================================

# signing — cryptographic primitives (SHA, HMAC, Base64, hex) from the
# header-only SDK (also used by signing-enabled drivers).
add_unit_test(signing SOURCE plugin_support/signing_test.cpp)
target_link_libraries(test_signing PRIVATE yaddnsc_plugin_sdk_crypto OpenSSL::Crypto)

if(YADDNSC_HAS_SDK_XML_COMPONENT)
    add_unit_test(xml_raii SOURCE sdk/xml_raii_test.cpp)
    target_link_libraries(test_xml_raii PRIVATE yaddnsc_plugin_sdk_xml)
endif()
if(YADDNSC_HAS_SDK_XML_COMPONENT)
    add_unit_test(xml_raii_allocation SOURCE sdk/xml_raii_allocation_test.cpp)
    target_link_libraries(test_xml_raii_allocation PRIVATE yaddnsc_plugin_sdk_xml test_allocation_fault)
endif()
