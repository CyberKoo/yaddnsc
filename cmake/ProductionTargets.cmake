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
#
# Stage 3 removed the legacy synchronous tree; the surviving transport-adjacent
# codec, URI and CA-discovery units were migrated under src/infrastructure/net/.
# ==============================================================================

# Coroutine runtime core — the loop (poll fd table, timer heap, ready queue,
# cross-thread inbox), Task<T>, structured scopes, cancellation combinators,
# cancellable sleeps, AsyncMutex, offload + SerialLane and signals. The offload
# pool is BS::thread_pool, reused rather than hand-rolled (see the pool note in
# src/infrastructure/coro/loop.h). It is named in the module's loop.h, so
# BS_thread_pool is PUBLIC here; everything else is the standard library and
# POSIX.
add_library(yaddnsc_coro STATIC
    src/infrastructure/coro/cancel_scope.cpp
    src/infrastructure/coro/loop.cpp
)
yaddnsc_production_module(yaddnsc_coro)
# PUBLIC: loop.h exposes the pool type that offload() submits to.
target_link_libraries(yaddnsc_coro PUBLIC BS_thread_pool)

# Coroutine transport layer — TCP, TLS, UDP, the sockaddr codec (SocketAddr) and
# CA discovery. Targets are already-resolved InetAddress values; resolving a
# hostname is the resolver's business, not the transport's. OpenSSL is PUBLIC
# because tls_stream.h publishes the SSL_CTX/SSL ownership types.
add_library(yaddnsc_net STATIC
    src/infrastructure/net/detail/socket_ops.cpp
    src/infrastructure/net/socket_addr.cpp
    src/infrastructure/net/tcp_stream.cpp
    src/infrastructure/net/tls_stream.cpp
    src/infrastructure/net/udp_socket.cpp
    src/infrastructure/net/tls/cert_util.cpp
    src/infrastructure/net/http/uri.cpp
)
yaddnsc_production_module(yaddnsc_net)
target_link_libraries(yaddnsc_net
    PUBLIC yaddnsc_coro yaddnsc_domain OpenSSL::SSL OpenSSL::Crypto
    PRIVATE spdlog::spdlog yaddnsc_fmt
)

# Coroutine application protocols — the HTTP client, the URI codec and the DNS
# subsystem. They share one archive because they are mutually dependent:
# resolving a URL host needs the DNS bootstrap resolver, and DoH/DoT are DNS
# resolvers built on the HTTP client and on the TLS stream.
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
    PRIVATE yaddnsc_dns_classic picohttpparser spdlog::spdlog magic_enum yaddnsc_fmt
)

# Domain layer — pure rules and value types (no I/O, threading, or clock reads).
# std::chrono time/duration value types are permitted.
# Also owns dns/error.cpp: DnsError is the port-level shared error vocabulary.
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

# Classic DNS wire logic — wire format, response parser/validator and the
# resolv.conf reader. No sockets: the coroutine exchanges live in
# yaddnsc_coro_io.
add_library(yaddnsc_dns_classic STATIC
    src/infrastructure/dns/validator.cpp
    src/infrastructure/dns/parser.cpp
    src/infrastructure/dns/wire/builder.cpp
    src/infrastructure/dns/resolv_conf.cpp
)
yaddnsc_production_module(yaddnsc_dns_classic)
target_link_libraries(yaddnsc_dns_classic
    PUBLIC yaddnsc_domain
    # OpenSSL::Crypto: the wire builder draws random query IDs via RAND_bytes.
    PRIVATE spdlog::spdlog magic_enum OpenSSL::Crypto yaddnsc_fmt
)

# Application layer — the retained use cases and ports. Deliberately free of
# CLI11, OpenSSL, Glaze, spdlog and dlopen: logging goes through the
# ports/log.h facade.
add_library(yaddnsc_application STATIC
    src/application/diagnostics.cpp
    src/application/environment_validator.cpp
)
yaddnsc_production_module(yaddnsc_application)
target_link_libraries(yaddnsc_application
    PUBLIC yaddnsc_domain yaddnsc_fmt
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
# config/config.h exposes glaze types in its interface; the static validator
# uses the URI codec for resolver-address checks.
target_link_libraries(yaddnsc_config_infrastructure
    PUBLIC yaddnsc_domain yaddnsc_net glaze::glaze yaddnsc_fmt
    PRIVATE spdlog::spdlog
)

# IP-source infrastructure — the interface enumeration cache, the mDNS response
# filter and the NetworkInterfaces port implementation.
add_library(yaddnsc_ip_source_infrastructure STATIC
    src/infrastructure/ip_source/iface_util.cpp
    src/infrastructure/ip_source/mdns_response.cpp
    src/infrastructure/ip_source/system_network_interfaces.cpp
)
yaddnsc_production_module(yaddnsc_ip_source_infrastructure)
target_link_libraries(yaddnsc_ip_source_infrastructure
    PUBLIC yaddnsc_domain
    PRIVATE yaddnsc_dns_classic spdlog::spdlog yaddnsc_fmt
)

# Plugin host infrastructure — the v1 alpha C ABI loader, catalog and leases.
add_library(yaddnsc_plugin_infrastructure STATIC
    src/infrastructure/plugin/shared_library.cpp
    src/infrastructure/plugin/plugin_loader.cpp
    src/infrastructure/plugin/driver_instance.cpp
    src/infrastructure/plugin/driver_catalog.cpp
)
yaddnsc_production_module(yaddnsc_plugin_infrastructure)
target_link_libraries(yaddnsc_plugin_infrastructure
    PUBLIC yaddnsc_application
    PRIVATE spdlog::spdlog yaddnsc_fmt ${CMAKE_DL_LIBS}
)

# Coroutine plugin bridge — the worker ↔ loop HTTP bridge, Host Services over
# it, and the coroutine driver gateway (src/infrastructure/plugin/coro/).
add_library(yaddnsc_coro_plugin STATIC
    src/infrastructure/plugin/coro/bridge.cpp
    src/infrastructure/plugin/coro/host_services.cpp
    src/infrastructure/plugin/coro/driver_gateway.cpp
)
yaddnsc_production_module(yaddnsc_coro_plugin)
target_link_libraries(yaddnsc_coro_plugin
    PUBLIC yaddnsc_plugin_infrastructure
    PRIVATE yaddnsc_coro_io yaddnsc_net spdlog::spdlog yaddnsc_fmt
)

# Coroutine application layer — the per-subdomain scheduling coroutines, the run
# root and the coroutine diagnostic handlers (src/application/coro/). Built on
# the coroutine runtime and the domain layer only; it names application ports,
# never a concrete infrastructure type.
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

# Coroutine IP sources — interface / HTTP / mDNS, plus the app::IpSourcePort
# adapter. It reuses the interface-enumeration cache and the mDNS response
# filter from yaddnsc_ip_source_infrastructure.
add_library(yaddnsc_coro_ip_source STATIC
    src/infrastructure/ip_source/coro/iface.cpp
    src/infrastructure/ip_source/coro/http.cpp
    src/infrastructure/ip_source/coro/mdns.cpp
    src/infrastructure/ip_source/coro/adapter.cpp
)
yaddnsc_production_module(yaddnsc_coro_ip_source)
target_link_libraries(yaddnsc_coro_ip_source
    PUBLIC yaddnsc_domain yaddnsc_coro_io yaddnsc_coro
    PRIVATE yaddnsc_ip_source_infrastructure spdlog::spdlog yaddnsc_fmt
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

add_library(yaddnsc_logging_adapter STATIC
    src/infrastructure/logging/spdlog_logger.cpp
)
yaddnsc_production_module(yaddnsc_logging_adapter)
target_link_libraries(yaddnsc_logging_adapter
    PUBLIC yaddnsc_application
    PRIVATE spdlog::spdlog
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
        yaddnsc_dns_classic
        yaddnsc_ip_source_infrastructure
        yaddnsc_plugin_infrastructure
        yaddnsc_plugin_loader_adapter
        yaddnsc_logging_adapter
        yaddnsc_net
        yaddnsc_cli_adapter
        yaddnsc_coro_application
        yaddnsc_coro_ip_source
        yaddnsc_coro_plugin
    PRIVATE spdlog::spdlog magic_enum yaddnsc_fmt
)
