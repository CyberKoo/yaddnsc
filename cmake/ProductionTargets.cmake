# ==============================================================================
# Production module targets — static libraries with an explicit dependency
# direction:
#
#   domain ← application ← infrastructure/adapters ← composition ← executable
#
# Inter-module links are PUBLIC so tests can link a single module and get
# its transitive closure; third-party dependencies are PRIVATE to the
# modules that actually use them. No static-initialization registration
# exists anywhere in the tree, so plain static archives link correctly.
# ==============================================================================

# TLS support infrastructure (CA certificate discovery).
add_library(yaddnsc_tls_support STATIC
    src/infrastructure/network/tls/cert_util.cpp
)
yaddnsc_production_module(yaddnsc_tls_support)
target_link_libraries(yaddnsc_tls_support PRIVATE OpenSSL::Crypto spdlog::spdlog)

# Domain layer — pure rules and value types (no I/O, threading, or clock reads).
# std::chrono time/duration value types are permitted.
# Also owns dns/error.cpp: DnsError is the port-level shared error vocabulary
# (src/domain/error/dns_error.h), so its stringification must be linkable by the
# application layer without pulling in the DNS infrastructure stack.
add_library(yaddnsc_domain STATIC
    src/domain/update/schedule_queue.cpp
    src/domain/update/update_decision.cpp
    src/domain/network/inet_address.cpp
    src/domain/error/dns_error.cpp
    src/domain/address_policy.cpp
    src/domain/fqdn.cpp
)
yaddnsc_production_module(yaddnsc_domain)
# Domain has no third-party or infrastructure dependency.

# Network infrastructure: OS socket wrappers, devices, and URI handling.
add_library(yaddnsc_network_infrastructure STATIC
    src/infrastructure/network/net_devices.cpp
    src/infrastructure/network/socket.cpp
    src/infrastructure/network/socket_addr.cpp
    src/infrastructure/network/socket_exception.cpp
    src/infrastructure/network/uri.cpp
)
yaddnsc_production_module(yaddnsc_network_infrastructure)
target_link_libraries(yaddnsc_network_infrastructure
    PUBLIC yaddnsc_domain
    PRIVATE spdlog::spdlog yaddnsc_fmt
)

# Classic DNS resolution: wire format, response parser/validator and the
# self-contained UDP/TCP resolver (no libresolv). Lives below the transport
# layer so SocketStream can bootstrap hostnames without a dependency cycle.
add_library(yaddnsc_dns_classic STATIC
    src/infrastructure/dns/validator.cpp
    src/infrastructure/dns/parser.cpp
    src/infrastructure/dns/wire/builder.cpp
    src/infrastructure/dns/resolver/classic.cpp
    src/infrastructure/dns/bootstrap.cpp
    src/infrastructure/dns/resolv_conf.cpp
)
yaddnsc_production_module(yaddnsc_dns_classic)
target_link_libraries(yaddnsc_dns_classic
    PUBLIC yaddnsc_domain yaddnsc_network_infrastructure
    # OpenSSL::Crypto: the wire builder draws random query IDs via RAND_bytes.
    PRIVATE spdlog::spdlog magic_enum OpenSSL::Crypto yaddnsc_fmt
)

# Network transport infrastructure (net::transport): TCP/TLS streams over OpenSSL.
# OpenSSL is PUBLIC: the public tls_stream.h includes <openssl/ssl.h>, so every
# consumer needs the OpenSSL include path (Homebrew OpenSSL on macOS lives
# outside the default search path).
add_library(yaddnsc_network_transport STATIC
    src/infrastructure/network/transport/detail/socket_stream.cpp
    src/infrastructure/network/transport/tls_stream.cpp
    src/infrastructure/network/transport/tcp_stream.cpp
)
yaddnsc_production_module(yaddnsc_network_transport)
target_link_libraries(yaddnsc_network_transport
    PUBLIC yaddnsc_network_infrastructure yaddnsc_tls_support OpenSSL::SSL OpenSSL::Crypto
    PRIVATE spdlog::spdlog yaddnsc_fmt yaddnsc_dns_classic
)

# HTTP client infrastructure (net::http) — used by the HTTP IP source
# (PersistentClient), DoH and the driver Host Services.
add_library(yaddnsc_http_infrastructure STATIC
    src/infrastructure/network/http/protocol/exchange.cpp
    src/infrastructure/network/http/protocol/wire.cpp
    src/infrastructure/network/http/redirect.cpp
    src/infrastructure/network/http/stream_factory.cpp
    src/infrastructure/network/http/wire_request.cpp
    src/infrastructure/network/http/session.cpp
    src/infrastructure/network/http/client.cpp
    src/infrastructure/network/http/persistent_client.cpp
)
yaddnsc_production_module(yaddnsc_http_infrastructure)
target_link_libraries(yaddnsc_http_infrastructure
    PUBLIC yaddnsc_network_transport
    PRIVATE picohttpparser spdlog::spdlog yaddnsc_fmt
)

# Application layer — use cases and the scheduling loop over the ports.
# Deliberately free of CLI11, OpenSSL, Glaze, spdlog and dlopen: logging
# goes through the ports/log.h facade.
add_library(yaddnsc_application STATIC
    src/application/update_workflow.cpp
    src/application/scheduler_runner.cpp
    src/application/pool_task_executor.cpp
    src/application/run_lifecycle.cpp
    src/application/diagnostics.cpp
    src/application/environment_validator.cpp
)
yaddnsc_production_module(yaddnsc_application)
target_link_libraries(yaddnsc_application
    PUBLIC yaddnsc_domain yaddnsc_fmt BS_thread_pool
    PRIVATE magic_enum
)

# Configuration infrastructure — JSON parsing, normalization, validation.
add_library(yaddnsc_config_infrastructure STATIC
    src/infrastructure/config/config.cpp
    src/infrastructure/config/normalizer.cpp
    src/infrastructure/config/diagnostics/parse_diagnostic.cpp
    src/infrastructure/config/diagnostics/error_adapter.cpp
    src/infrastructure/config/diagnostics/locator.cpp
    src/infrastructure/config/diagnostics/schema.cpp
    src/infrastructure/config/diagnostics/decision.cpp
    src/infrastructure/config/diagnostics/renderer.cpp
    src/infrastructure/config/static_validator.cpp
)
yaddnsc_production_module(yaddnsc_config_infrastructure)
# config/config.h exposes glaze types in its interface.
target_link_libraries(yaddnsc_config_infrastructure
    PUBLIC yaddnsc_domain yaddnsc_network_infrastructure glaze::glaze yaddnsc_fmt
    PRIVATE spdlog::spdlog
)

# DNS infrastructure — wire format, parsers, classic/DoT/DoH resolvers.
# (dns/error.cpp lives in yaddnsc_domain: DnsError is port-level vocabulary.)
# DNS infrastructure: resolver backends (DoH / DoT), dispatch strategies and
# the resolver factory/catalog. The classic UDP/TCP resolver, wire format and
# parser live in yaddnsc_dns_classic (below the transport layer).
add_library(yaddnsc_dns_infrastructure STATIC
    src/infrastructure/dns/resolver/doh.cpp
    src/infrastructure/dns/resolver/dot.cpp
    src/infrastructure/dns/factory.cpp
    src/infrastructure/dns/resolver_catalog.cpp
    src/infrastructure/dns/dispatcher.cpp
)
yaddnsc_production_module(yaddnsc_dns_infrastructure)
target_link_libraries(yaddnsc_dns_infrastructure
    PUBLIC yaddnsc_domain yaddnsc_dns_classic yaddnsc_http_infrastructure
    PRIVATE spdlog::spdlog magic_enum OpenSSL::Crypto yaddnsc_fmt
)

# IP-source infrastructure — interface / HTTP / mDNS sources + factory.
add_library(yaddnsc_ip_source_infrastructure STATIC
    src/infrastructure/ip_source/iface_util.cpp
    src/infrastructure/ip_source/iface.cpp
    src/infrastructure/ip_source/http.cpp
    src/infrastructure/ip_source/mdns.cpp
    src/infrastructure/ip_source/mdns_response.cpp
    src/infrastructure/ip_source/factory.cpp
    src/infrastructure/ip_source/adapter.cpp
)
yaddnsc_production_module(yaddnsc_ip_source_infrastructure)
target_link_libraries(yaddnsc_ip_source_infrastructure
    PUBLIC yaddnsc_network_infrastructure yaddnsc_dns_infrastructure
    PRIVATE spdlog::spdlog yaddnsc_fmt
)

# Plugin host infrastructure — the v1 alpha C ABI loader, catalog, Host
# Services and driver gateway.
add_library(yaddnsc_plugin_infrastructure STATIC
    src/infrastructure/plugin/shared_library.cpp
    src/infrastructure/plugin/plugin_loader.cpp
    src/infrastructure/plugin/driver_instance.cpp
    src/infrastructure/plugin/driver_catalog.cpp
    src/infrastructure/plugin/host_services.cpp
    src/infrastructure/plugin/abi_driver_gateway.cpp
)
yaddnsc_production_module(yaddnsc_plugin_infrastructure)
target_link_libraries(yaddnsc_plugin_infrastructure
    PUBLIC yaddnsc_application
    PRIVATE spdlog::spdlog yaddnsc_fmt ${CMAKE_DL_LIBS}
)

# Concrete adapters stay separate so their target dependencies express their
# actual ports and infrastructure requirements.
add_library(yaddnsc_plugin_loader_adapter STATIC
    src/infrastructure/plugin/driver_loader.cpp
)
yaddnsc_production_module(yaddnsc_plugin_loader_adapter)
target_link_libraries(yaddnsc_plugin_loader_adapter
    PUBLIC yaddnsc_plugin_infrastructure
    PRIVATE spdlog::spdlog yaddnsc_fmt
)

add_library(yaddnsc_process_adapter STATIC
    src/infrastructure/process/signal_watcher.cpp
)
yaddnsc_production_module(yaddnsc_process_adapter)
target_link_libraries(yaddnsc_process_adapter
    PRIVATE spdlog::spdlog
)

add_library(yaddnsc_logging_adapter STATIC
    src/infrastructure/logging/spdlog_logger.cpp
)
yaddnsc_production_module(yaddnsc_logging_adapter)
target_link_libraries(yaddnsc_logging_adapter
    PUBLIC yaddnsc_application
    PRIVATE spdlog::spdlog
)

add_library(yaddnsc_time_adapter STATIC
    src/infrastructure/time/steady_clock.cpp
)
yaddnsc_production_module(yaddnsc_time_adapter)
target_link_libraries(yaddnsc_time_adapter PUBLIC yaddnsc_application)

add_library(yaddnsc_network_adapter STATIC
    src/infrastructure/network/system_network_interfaces.cpp
)
yaddnsc_production_module(yaddnsc_network_adapter)
target_link_libraries(yaddnsc_network_adapter
    PUBLIC yaddnsc_application yaddnsc_ip_source_infrastructure
)

# CLI adapter — argument parsing and output presentation.
add_library(yaddnsc_cli_adapter STATIC
    src/cli/parser.cpp
    src/cli/presenter.cpp
)
yaddnsc_production_module(yaddnsc_cli_adapter)
target_link_libraries(yaddnsc_cli_adapter
    PUBLIC yaddnsc_application
    PRIVATE CLI11::CLI11 magic_enum yaddnsc_fmt
)

# Composition root — assembles concrete dependencies and dispatches commands.
add_library(yaddnsc_composition STATIC
    src/composition/bootstrap.cpp
)
yaddnsc_production_module(yaddnsc_composition)
target_link_libraries(yaddnsc_composition
    PUBLIC
        yaddnsc_domain
        yaddnsc_application
        yaddnsc_config_infrastructure
        yaddnsc_dns_infrastructure
        yaddnsc_ip_source_infrastructure
        yaddnsc_plugin_infrastructure
        yaddnsc_plugin_loader_adapter
        yaddnsc_process_adapter
        yaddnsc_logging_adapter
        yaddnsc_time_adapter
        yaddnsc_network_adapter
        yaddnsc_network_infrastructure
        yaddnsc_network_transport
        yaddnsc_http_infrastructure
        yaddnsc_tls_support
        yaddnsc_cli_adapter
    PRIVATE spdlog::spdlog magic_enum yaddnsc_fmt
)
