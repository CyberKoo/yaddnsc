# ==============================================================================
# Architecture guard — static boundary checks over the source tree
# ==============================================================================
# Registered as the ctest test `architecture_guard`. Textual include/pattern
# checks only — deliberately no full static-analysis framework.
#
# Usage: cmake -DPROJECT_SOURCE_DIR=<repo-root> -P ArchitectureGuard.cmake
# ==============================================================================

if (NOT DEFINED PROJECT_SOURCE_DIR)
    message(FATAL_ERROR "Run with: cmake -DPROJECT_SOURCE_DIR=<repo-root> -P ArchitectureGuard.cmake")
endif ()

set(violations "")

# guard_check(<description> <line_regex> <glob> [<glob>...])
# Appends one violation per line matching <line_regex> in the globbed files.
# Comment lines (//, /*, *) are ignored — matching prose is not a violation.
function(guard_check description line_regex)
    file(GLOB_RECURSE files RELATIVE ${PROJECT_SOURCE_DIR} ${ARGN})
    foreach (f ${files})
        file(STRINGS ${PROJECT_SOURCE_DIR}/${f} lines REGEX "${line_regex}")
        foreach (line ${lines})
            if (line MATCHES "^[ \t]*(//|/\\*|\\*)")
                continue()
            endif ()
            string(STRIP "${line}" stripped)
            set(violations "${violations}\n  ${f}: ${description}\n      ${stripped}")
        endforeach ()
    endforeach ()
    set(violations "${violations}" PARENT_SCOPE)
endfunction()

set(INC_RE "^[ \t]*#[ \t]*include[ \t]*")

# ------------------------------------------------------------------------------
# 1. domain: no infrastructure, third-party libraries or cancellation plumbing
# ------------------------------------------------------------------------------
guard_check("domain must not include infrastructure/Glaze/spdlog/CLI11/CancellationToken"
    "${INC_RE}[<\"](infrastructure/|glaze/|spdlog/|CLI/|[^\">]*[Cc]ancellation[Tt]oken[^\">]*)"
    ${PROJECT_SOURCE_DIR}/src/domain/*.h
    ${PROJECT_SOURCE_DIR}/src/domain/*.hpp
    ${PROJECT_SOURCE_DIR}/src/domain/*.cpp)

# ------------------------------------------------------------------------------
# 2. application: no infrastructure implementation headers or third-party I/O
# ------------------------------------------------------------------------------
guard_check("application must not include infrastructure/spdlog/Glaze/CLI11/OpenSSL/dlopen"
    "${INC_RE}[<\"](infrastructure/|composition/|cli/|spdlog/|glaze/|CLI/|openssl/|dlfcn\\.h)"
    ${PROJECT_SOURCE_DIR}/src/application/*.h
    ${PROJECT_SOURCE_DIR}/src/application/*.hpp
    ${PROJECT_SOURCE_DIR}/src/application/*.cpp)

# ------------------------------------------------------------------------------
# 3. plugins: SDK only — no host src/ modules, no legacy utility headers,
#    no spdlog (logging goes through Host Services)
# ------------------------------------------------------------------------------
guard_check("plugins must not include host src/ module headers"
    "${INC_RE}[<\"](\\.\\./|(core|application|infrastructure|domain|config|network|http_client|cli|composition|dns|ip_source|util|support)/)"
    ${PROJECT_SOURCE_DIR}/driver/*.h
    ${PROJECT_SOURCE_DIR}/driver/*.hpp
    ${PROJECT_SOURCE_DIR}/driver/*.cpp)

guard_check("plugins must not include legacy utility headers (use yaddnsc/sdk/*)"
    "${INC_RE}[<\"](CORE_LOG[^\">]*|core_logger[^\">]*|form_encode\\.h|xml_raii\\.h|uri\\.h|fmt\\.hpp|string_util\\.hpp|HttpClient[^\">]*|spdlog/[^\">]*)[\">]"
    ${PROJECT_SOURCE_DIR}/driver/*.h
    ${PROJECT_SOURCE_DIR}/driver/*.hpp
    ${PROJECT_SOURCE_DIR}/driver/*.cpp)

# ------------------------------------------------------------------------------
# 4. driver_abi.h stays pure C (C standard headers only)
# ------------------------------------------------------------------------------
set(ABI_HEADER ${PROJECT_SOURCE_DIR}/include/yaddnsc/sdk/driver_abi.h)
file(STRINGS ${ABI_HEADER} abi_includes REGEX "${INC_RE}[<\"]")
foreach (line ${abi_includes})
    if (NOT line MATCHES "^[ \t]*#[ \t]*include[ \t]*<(stddef\\.h|stdint\\.h|stdbool\\.h)>[ \t]*$")
        string(STRIP "${line}" stripped)
        set(violations "${violations}\n  include/yaddnsc/sdk/driver_abi.h: C ABI header must include only C standard headers\n      ${stripped}")
    endif ()
endforeach ()

# ------------------------------------------------------------------------------
# 5. no global resolver registry (instance-level ResolverCatalog instead)
# ------------------------------------------------------------------------------
guard_check("global resolver registry must not reappear (use ResolverCatalog instances)"
    "DnsResolverRegistry"
    ${PROJECT_SOURCE_DIR}/src/*.h
    ${PROJECT_SOURCE_DIR}/src/*.hpp
    ${PROJECT_SOURCE_DIR}/src/*.cpp)

# ------------------------------------------------------------------------------
# 6. no parallel HTTP types (net::http types + the HttpClient port are the
#    single implementation; the plugin SDK's own ABI DTOs are the sanctioned
#    plugin-side surface and are excluded)
# ------------------------------------------------------------------------------
file(GLOB_RECURSE http_check_files RELATIVE ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/*.h
    ${PROJECT_SOURCE_DIR}/src/*.hpp
    ${PROJECT_SOURCE_DIR}/src/*.cpp
    ${PROJECT_SOURCE_DIR}/include/*.h
    ${PROJECT_SOURCE_DIR}/include/*.hpp)
list(FILTER http_check_files EXCLUDE REGEX "^include/yaddnsc/sdk/")
foreach (f ${http_check_files})
    file(STRINGS ${PROJECT_SOURCE_DIR}/${f} lines REGEX "(class|struct)[ \t]+(HttpTransport|HttpRequest|HttpResponse)[ \t\n{]")
    foreach (line ${lines})
        if (line MATCHES "^[ \t]*(//|/\\*|\\*)")
            continue()
        endif ()
        string(STRIP "${line}" stripped)
        set(violations "${violations}\n  ${f}: no parallel HTTP types (use src/infrastructure/network/http/types.h + src/infrastructure/network/http/client_port.h)\n      ${stripped}")
    endforeach ()
endforeach ()

# ------------------------------------------------------------------------------
# 7. host code must not cross the plugin boundary with the plugin-side C++
#    helper (the host speaks driver_abi.h only)
# ------------------------------------------------------------------------------
guard_check("host code must not include the plugin-side SDK helper driver.hpp"
    "${INC_RE}[<\"]yaddnsc/sdk/driver\\.hpp"
    ${PROJECT_SOURCE_DIR}/src/*.h
    ${PROJECT_SOURCE_DIR}/src/*.hpp
    ${PROJECT_SOURCE_DIR}/src/*.cpp)

# ------------------------------------------------------------------------------
# 8. no -rdynamic / export-dynamic backfill: plugins must not resolve host
#    symbols (comment lines are ignored)
# ------------------------------------------------------------------------------
file(GLOB_RECURSE cmake_files RELATIVE ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/CMakeLists.txt
    ${PROJECT_SOURCE_DIR}/cmake/*.cmake
    ${PROJECT_SOURCE_DIR}/driver/*/CMakeLists.txt
    ${PROJECT_SOURCE_DIR}/test/*/CMakeLists.txt
    ${PROJECT_SOURCE_DIR}/plugin_support/crypto/CMakeLists.txt)
# The guard script itself contains these patterns as literals.
list(FILTER cmake_files EXCLUDE REGEX "^cmake/ArchitectureGuard\\.cmake$")
foreach (f ${cmake_files})
    file(STRINGS ${PROJECT_SOURCE_DIR}/${f} lines REGEX "rdynamic|export-dynamic|dynamic_lookup|ENABLE_EXPORTS")
    foreach (line ${lines})
        if (NOT line MATCHES "^[ \t]*#")
            string(STRIP "${line}" stripped)
            set(violations "${violations}\n  ${f}: plugins must not resolve host symbols (no -rdynamic/export-dynamic)\n      ${stripped}")
        endif ()
    endforeach ()
endforeach ()

# ------------------------------------------------------------------------------
# 9. plugins must not redefine shared string utilities — the single
#    implementation lives in include/yaddnsc/util/ (exposed to drivers via
#    yaddnsc/sdk/*); a return-type prefix keeps call sites from matching
# ------------------------------------------------------------------------------
guard_check("plugins must not redefine shared string utilities (use yaddnsc/sdk/string_util.hpp, yaddnsc/sdk/url_encode.hpp)"
    "^[ \t]*(static[ \t]+|inline[ \t]+)*(void|bool|std::string|std::string_view)[ \t]+(ltrim|rtrim|trim|replace_all|url_encode)[ \t]*\\("
    ${PROJECT_SOURCE_DIR}/driver/*/*.h
    ${PROJECT_SOURCE_DIR}/driver/*/*.hpp
    ${PROJECT_SOURCE_DIR}/driver/*/*.cpp)

# ------------------------------------------------------------------------------
# 10. dns_classic stays below the transport layer. Its sources are the
#     classic UDP and TCP exchanges and the wire format — the pieces that must
#     not pull in the HTTP/transport/plugin machinery (TcpConnection depends
#     on this target; a back-edge would create a cycle). Bootstrap's TCP
#     fallback uses the shared Socket transfer in network infrastructure, not
#     TcpStream. dns/resolver/ is the other half: classic/doh/dot are facades
#     above transport and use TcpStream, so resolver/classic.cpp is excluded
#     from this check. The pattern is anchored to /classic.cpp, which leaves every
#     file under dns/classic/ — including classic_udp.cpp and classic_tcp.cpp —
#     covered.
# ------------------------------------------------------------------------------
file(GLOB_RECURSE dns_classic_files RELATIVE ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/infrastructure/dns/*.h
    ${PROJECT_SOURCE_DIR}/src/infrastructure/dns/*.hpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/dns/*.cpp)
list(FILTER dns_classic_files EXCLUDE REGEX "(doh|dot|dispatcher|factory|resolver_catalog|tls_options|connect_error)\\.|/classic\\.cpp$")
foreach (f ${dns_classic_files})
    file(STRINGS ${PROJECT_SOURCE_DIR}/${f} lines REGEX "${INC_RE}[<\"](infrastructure/network/(http|transport)/|infrastructure/plugin/|openssl/)")
    foreach (line ${lines})
        if (line MATCHES "^[ \t]*(//|/\\*|\\*)")
            continue()
        endif ()
        string(STRIP "${line}" stripped)
        set(violations "${violations}\n  ${f}: dns_classic must not depend on HTTP/transport/plugin/OpenSSL (layering cycle)\n      ${stripped}")
    endforeach ()
endforeach ()

# ------------------------------------------------------------------------------
# 11. thread pools are an implementation detail: offload() is the only gateway.
#     The coroutine runtime owns the single pool business code may reach
#     (src/infrastructure/coro/ names BS::thread_pool to implement offload and
#     SerialLane), and the pre-existing application executor still names it while
#     the synchronous backend exists. Anywhere else in src/ a direct include is a
#     layering violation: route the work through coro::offload instead of
#     reaching for a thread pool. The plugin boundary (driver/) is deliberately
#     not checked — plugins are a separate binary boundary and may use their own
#     pool.
# ------------------------------------------------------------------------------
file(GLOB_RECURSE thread_pool_check_files RELATIVE ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/*.h
    ${PROJECT_SOURCE_DIR}/src/*.hpp
    ${PROJECT_SOURCE_DIR}/src/*.cpp)
list(FILTER thread_pool_check_files EXCLUDE REGEX "^src/infrastructure/coro/")
list(FILTER thread_pool_check_files EXCLUDE REGEX "^src/application/pool_task_executor\\.cpp$")
foreach (f ${thread_pool_check_files})
    file(STRINGS ${PROJECT_SOURCE_DIR}/${f} lines REGEX "${INC_RE}[<\"]BS_thread_pool")
    foreach (line ${lines})
        if (line MATCHES "^[ \t]*(//|/\\*|\\*)")
            continue()
        endif ()
        string(STRIP "${line}" stripped)
        set(violations "${violations}\n  ${f}: thread pools must stay behind coro::offload (do not include BS_thread_pool directly)\n      ${stripped}")
    endforeach ()
endforeach()

# ------------------------------------------------------------------------------
# 12. The coroutine transport (src/infrastructure/net/) must not fall back on the
#     legacy blocking transport it replaces: network/transport/ and the blocking
#     Socket/poll machinery. Reusing the pure address codec
#     (network/socket_addr.h) and CA discovery (network/tls/cert_util.h) is
#     intended and allowed; the blocking/whole-operation-timeout machinery is
#     not, because stage 3 deletes that tree.
# ------------------------------------------------------------------------------
guard_check("coroutine transport must not depend on the legacy blocking transport"
    "${INC_RE}[<\"](infrastructure/network/(transport/|socket\\.h|socket_exception\\.h|tcp_transfer\\.h))"
    ${PROJECT_SOURCE_DIR}/src/infrastructure/net/*.h
    ${PROJECT_SOURCE_DIR}/src/infrastructure/net/*.hpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/net/*.cpp)

# ------------------------------------------------------------------------------
# 13. The coroutine DNS subsystem (src/infrastructure/dns/coro/) must not fall
#     back on the synchronous resolvers it replaces: the resolver facades, their
#     classic UDP/TCP transports, the legacy dispatcher/bootstrap and the factory
#     catalog. The pure wire layer (types, parser, validator, wire/, util.hpp,
#     resolv_conf) stays reusable in place.
# ------------------------------------------------------------------------------
guard_check("coroutine DNS must not depend on the legacy synchronous resolvers"
    "${INC_RE}[<\"](infrastructure/dns/(resolver/|classic/|dispatcher\.h|bootstrap\.h|factory\.h|resolver_catalog\.h))"
    ${PROJECT_SOURCE_DIR}/src/infrastructure/dns/coro/*.h
    ${PROJECT_SOURCE_DIR}/src/infrastructure/dns/coro/*.hpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/dns/coro/*.cpp)

# ------------------------------------------------------------------------------
# 14. The coroutine plugin bridge (src/infrastructure/plugin/coro/) must not fall
#     back on the synchronous host it replaces: the blocking ABI gateway and its
#     token-threaded Host Services. The reusable ABI plumbing (plugin_loader,
#     driver_catalog, driver_instance, shared_library) stays in place.
# ------------------------------------------------------------------------------
guard_check("coroutine plugin bridge must not depend on the synchronous driver host"
    "${INC_RE}[<\"]infrastructure/plugin/(abi_driver_gateway|host_services)\\.h"
    ${PROJECT_SOURCE_DIR}/src/infrastructure/plugin/coro/*.h
    ${PROJECT_SOURCE_DIR}/src/infrastructure/plugin/coro/*.hpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/plugin/coro/*.cpp)

# ------------------------------------------------------------------------------
# Verdict
# ------------------------------------------------------------------------------
if (violations)
    message(FATAL_ERROR "Architecture guard violations:${violations}\n")
endif ()

message(STATUS "Architecture guard: OK (domain/application/plugin/SDK/thread-pool/net-transport/dns-transport/coro-plugin/boundary checks passed)")
