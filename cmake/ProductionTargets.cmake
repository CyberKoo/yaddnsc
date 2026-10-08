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

# Coroutine runtime core — the loop (poll fd table, timer heap, ready queue,
# cross-thread inbox), Task<T>, structured scopes, cancellation combinators,
# cancellable sleeps, AsyncMutex, offload + SerialLane and signals. The offload
# pool is BS::thread_pool, reused rather than hand-rolled (see the pool note in
# src/infrastructure/coro/loop.h): a bundled dependency already present for the
# plugin executor. It is named in the module's loop.h, so BS_thread_pool is
# PUBLIC here. Everything else is the standard library and POSIX, which keeps
# this the bottom layer of the tree; stage 2 builds transport on top of it.
add_library(yaddnsc_coro STATIC
    src/infrastructure/coro/cancel_scope.cpp
    src/infrastructure/coro/loop.cpp
)
yaddnsc_production_module(yaddnsc_coro)
# PUBLIC: loop.h exposes the pool type that offload() submits to.
target_link_libraries(yaddnsc_coro PUBLIC BS_thread_pool)

# Coroutine transport layer — TCP, TLS and UDP objects over the coroutine
# runtime. Targets are already-resolved InetAddress values: hostname resolution
# needs the resolver port, which arrives in stage 2b. The legacy
# network/transport/ tree is untouched; this target exists to replace it in
# stage 3, which is why it is a separate module rather than an addition there.
# OpenSSL is PUBLIC because tls_stream.h publishes the SSL_CTX/SSL ownership
# types; yaddnsc_network_infrastructure is PRIVATE and provides only the
# sockaddr codec (SocketAddr) that bridges InetAddress to the POSIX API.
add_library(yaddnsc_net STATIC
    src/infrastructure/net/detail/socket_ops.cpp
    src/infrastructure/net/tcp_stream.cpp
    src/infrastructure/net/tls_stream.cpp
    src/infrastructure/net/udp_socket.cpp
)
yaddnsc_production_module(yaddnsc_net)
target_link_libraries(yaddnsc_net
    PUBLIC yaddnsc_coro yaddnsc_domain OpenSSL::SSL OpenSSL::Crypto
    PRIVATE yaddnsc_tls_support yaddnsc_network_infrastructure spdlog::spdlog yaddnsc_fmt
)

# Coroutine application protocols — the HTTP client and the DNS subsystem.
# They share one archive because they are mutually dependent: resolving a URL
# host needs the DNS bootstrap resolver, and DoH/DoT are DNS resolvers built on
# the HTTP client and on the TLS stream. Splitting them would need an injected
# resolver abstraction that only this archive would ever implement.
add_library(yaddnsc_coro_io STATIC
    src/infrastructure/net/stream.cpp
    src/infrastructure/net/http/protocol/wire.cpp
    src/infrastructure/net/http/protocol/exchange.cpp
    src/infrastructure/net/http/wire_request.cpp
    src/infrastructure/net/http/redirect.cpp
    src/infrastructure/net/http/transport.cpp
    src/infrastructure/net/http/session.cpp
    src/infrastructure/net/http/client.cpp
    src/infrastructure/net/http/persistent_client.cpp
    src/infrastructure/dns/coro/exchange.cpp
    src/infrastructure/dns/coro/bootstrap.cpp
    src/infrastructure/dns/coro/classic.cpp
    src/infrastructure/dns/coro/dot.cpp
    src/infrastructure/dns/coro/doh.cpp
    src/infrastructure/dns/coro/dispatcher.cpp
    src/infrastructure/dns/coro/factory.cpp
)
yaddnsc_production_module(yaddnsc_coro_io)
target_link_libraries(yaddnsc_coro_io
    PUBLIC yaddnsc_net yaddnsc_domain
    PRIVATE yaddnsc_dns_classic yaddnsc_network_infrastructure picohttpparser spdlog::spdlog magic_enum yaddnsc_fmt
)

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
    src/infrastructure/network/tcp_transfer.cpp
    src/infrastructure/network/uri.cpp
)
yaddnsc_production_module(yaddnsc_network_infrastructure)
target_link_libraries(yaddnsc_network_infrastructure
    PUBLIC yaddnsc_domain
    PRIVATE spdlog::spdlog yaddnsc_fmt
)

# Classic DNS resolution: wire format, response parser/validator, the classic
# UDP/TCP exchanges and bootstrap (no libresolv). Lives below the transport
# layer so TcpConnection can resolve hostnames without a dependency cycle.
# Directory and target boundaries differ: only the classic protocol's own
# transport sits under dns/classic/, while bootstrap, resolv_conf, parser,
# validator and wire/ are shared with the layers above and stay in dns/.
# The resolver facades above transport are in dns/resolver/ (classic.cpp,
# doh.cpp, dot.cpp) and belong to yaddnsc_dns_infrastructure.
add_library(yaddnsc_dns_classic STATIC
    src/infrastructure/dns/validator.cpp
    src/infrastructure/dns/parser.cpp
    src/infrastructure/dns/wire/builder.cpp
    src/infrastructure/dns/classic/classic_udp.cpp
    src/infrastructure/dns/classic/classic_tcp.cpp
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
    src/infrastructure/network/transport/detail/tcp_connection.cpp
    src/infrastructure/network/transport/detail/tls_io.cpp
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
# DNS infrastructure: resolver backends (DoH / DoT / classic TCP fallback),
# dispatch strategies and the resolver factory/catalog. The UDP exchange, wire
# format and parser live in yaddnsc_dns_classic (below the transport layer).
# classic.cpp is the upward edge: it may use TcpStream, and it is not
# part of yaddnsc_dns_classic.
add_library(yaddnsc_dns_infrastructure STATIC
    src/infrastructure/dns/resolver/classic.cpp
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

# Coroutine plugin bridge — the worker ↔ loop HTTP bridge, Host Services over
# it, and the coroutine driver gateway (src/infrastructure/plugin/coro/). It
# reuses PluginModule / DriverInstance / SharedLibrary / DriverCatalog in place
# and links only the coroutine protocol stack; the synchronous
# abi_driver_gateway / host_services remain for the synchronous backend and are
# deliberately not referenced (architecture_guard rule 14).
add_library(yaddnsc_coro_plugin STATIC
    src/infrastructure/plugin/coro/bridge.cpp
    src/infrastructure/plugin/coro/host_services.cpp
    src/infrastructure/plugin/coro/driver_gateway.cpp
)
yaddnsc_production_module(yaddnsc_coro_plugin)
target_link_libraries(yaddnsc_coro_plugin
    PUBLIC yaddnsc_plugin_infrastructure
    PRIVATE yaddnsc_coro_io yaddnsc_network_infrastructure spdlog::spdlog yaddnsc_fmt
)

# Coroutine application layer — the per-subdomain scheduling coroutines and the
# run root (src/application/coro/). Built on the coroutine runtime and the domain
# layer only; it names application ports, never a concrete infrastructure type
# (adapters live in yaddnsc_coro_ip_source / yaddnsc_coro_plugin / the DNS
# coroutine factory).
add_library(yaddnsc_coro_application STATIC
    src/application/coro/update_once.cpp
    src/application/coro/subdomain_loop.cpp
    src/application/coro/run_scheduler.cpp
    src/application/coro/diagnostics.cpp
)
yaddnsc_production_module(yaddnsc_coro_application)
target_link_libraries(yaddnsc_coro_application
    PUBLIC yaddnsc_domain yaddnsc_coro
    PRIVATE yaddnsc_application magic_enum yaddnsc_fmt
)

# Coroutine IP sources — interface / HTTP (native) and mDNS (offload transition),
# plus the app::IpSourcePort adapter. It reuses the legacy InterfaceUtil cache and
# the legacy mDNS source, which is the mDNS transition debt noted in the headers.
add_library(yaddnsc_coro_ip_source STATIC
    src/infrastructure/ip_source/coro/iface.cpp
    src/infrastructure/ip_source/coro/http.cpp
    src/infrastructure/ip_source/coro/mdns.cpp
    src/infrastructure/ip_source/coro/adapter.cpp
)
yaddnsc_production_module(yaddnsc_coro_ip_source)
target_link_libraries(yaddnsc_coro_ip_source
    PUBLIC yaddnsc_domain yaddnsc_coro_io yaddnsc_coro
    PRIVATE yaddnsc_ip_source_infrastructure yaddnsc_network_infrastructure spdlog::spdlog yaddnsc_fmt
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
        yaddnsc_coro_application
        yaddnsc_coro_ip_source
        yaddnsc_coro_plugin
    PRIVATE spdlog::spdlog magic_enum yaddnsc_fmt
)
