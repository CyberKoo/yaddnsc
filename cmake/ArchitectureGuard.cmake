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
# 2. application: no infrastructure implementation headers or third-party I/O.
#    The coroutine runtime (src/infrastructure/coro/) is the one allowed
#    infrastructure tree: it is the substrate the coroutine application layer is
#    built on (design §8), so src/application/ may include it. Every other
#    infrastructure/ include stays forbidden.
# ------------------------------------------------------------------------------
file(GLOB_RECURSE application_check_files RELATIVE ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/application/*.h
    ${PROJECT_SOURCE_DIR}/src/application/*.hpp
    ${PROJECT_SOURCE_DIR}/src/application/*.cpp)
foreach (f ${application_check_files})
    file(STRINGS ${PROJECT_SOURCE_DIR}/${f} lines REGEX
        "${INC_RE}[<\"](infrastructure/|composition/|cli/|spdlog/|glaze/|CLI/|openssl/|dlfcn\\.h)")
    foreach (line ${lines})
        if (line MATCHES "^[ \t]*(//|/\\*|\\*)")
            continue()
        endif ()
        if (line MATCHES "${INC_RE}[<\"]infrastructure/coro/")
            continue()
        endif ()
        string(STRIP "${line}" stripped)
        set(violations "${violations}\n  ${f}: application must not include infrastructure/spdlog/Glaze/CLI11/OpenSSL/dlopen\n      ${stripped}")
    endforeach ()
endforeach()

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
        set(violations "${violations}\n  ${f}: no parallel HTTP types (use src/infrastructure/net/http/types.h)\n      ${stripped}")
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
# 10. dns_classic (wire format, parser, validator, resolv.conf) stays below the
#     HTTP/transport layers: it must not pull in OpenSSL or the plugin tree. The
#     coroutine resolver facades (dns/coro/) are the layer above and are checked
#     separately by rule 2's spirit (they may use net/).
# ------------------------------------------------------------------------------
file(GLOB_RECURSE dns_classic_files RELATIVE ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/infrastructure/dns/*.h
    ${PROJECT_SOURCE_DIR}/src/infrastructure/dns/*.hpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/dns/*.cpp)
foreach (f ${dns_classic_files})
    file(STRINGS ${PROJECT_SOURCE_DIR}/${f} lines REGEX "${INC_RE}[<\"](infrastructure/plugin/|openssl/)")
    foreach (line ${lines})
        if (line MATCHES "^[ \t]*(//|/\\*|\\*)")
            continue()
        endif ()
        string(STRIP "${line}" stripped)
        set(violations "${violations}\n  ${f}: dns_classic must not depend on plugin/OpenSSL (layering)\n      ${stripped}")
    endforeach ()
endforeach ()

# ------------------------------------------------------------------------------
# 11. Concurrency is an implementation detail of the coroutine runtime. A direct
#     <thread>, <future> or BS::thread_pool include is allowed only in the
#     runtime (src/infrastructure/coro/) and in the single place that must hand
#     a result back across the synchronous plugin ABI boundary
#     (src/infrastructure/plugin/bridge.*). Everywhere else in the
#     application / infrastructure / composition layers, route the work through
#     coro::offload or a coro::Task. The plugin boundary (driver/) is a separate
#     binary boundary and is deliberately not checked; src/support/ is a generic
#     utility layer and is also not checked.
# ------------------------------------------------------------------------------
file(GLOB_RECURSE concurrency_check_files RELATIVE ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src/application/*.h
    ${PROJECT_SOURCE_DIR}/src/application/*.hpp
    ${PROJECT_SOURCE_DIR}/src/application/*.cpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/*.h
    ${PROJECT_SOURCE_DIR}/src/infrastructure/*.hpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/*.cpp
    ${PROJECT_SOURCE_DIR}/src/composition/*.h
    ${PROJECT_SOURCE_DIR}/src/composition/*.hpp
    ${PROJECT_SOURCE_DIR}/src/composition/*.cpp)
list(FILTER concurrency_check_files EXCLUDE REGEX "^src/infrastructure/coro/")
list(FILTER concurrency_check_files EXCLUDE REGEX "^src/infrastructure/plugin/bridge\\.(h|cpp)$")
foreach (f ${concurrency_check_files})
    file(STRINGS ${PROJECT_SOURCE_DIR}/${f} lines REGEX "${INC_RE}[<\"]((thread|future)>|BS_thread_pool)")
    foreach (line ${lines})
        if (line MATCHES "^[ \t]*(//|/\\*|\\*)")
            continue()
        endif ()
        string(STRIP "${line}" stripped)
        set(violations "${violations}\n  ${f}: threads, futures and thread pools belong to the coroutine runtime (use coro::offload / coro::Task)\n      ${stripped}")
    endforeach ()
endforeach()

# ------------------------------------------------------------------------------
# 12. The legacy per-operation cancellation token is gone: cancellation is scope
#     state reached through checkpoints. Neither the type nor its header may
#     reappear anywhere under src/.
# ------------------------------------------------------------------------------
guard_check("the legacy cancellation token must not reappear (cancellation is scope state)"
    "[Cc]ancellation(Token|Source)|cancellation_token\\.hpp"
    ${PROJECT_SOURCE_DIR}/src/*.h
    ${PROJECT_SOURCE_DIR}/src/*.hpp
    ${PROJECT_SOURCE_DIR}/src/*.cpp)

# ------------------------------------------------------------------------------
# 13. TLS trust is prepared before the loop starts (TlsContext::create), never
#     registered lazily for a handshake: OpenSSL's default-path lookup and the
#     X509_LOOKUP_* registrars read the filesystem on first use, which would
#     block the loop thread mid-connection. The eager X509_STORE_load_* family
#     is allowed — it populates the trust store at create() time, off-loop.
# ------------------------------------------------------------------------------
guard_check("TLS must not register lazy trust loading (build a TlsContext off-loop instead)"
    "SSL_CTX_set_default_verify_paths|X509_LOOKUP_"
    ${PROJECT_SOURCE_DIR}/src/*.h
    ${PROJECT_SOURCE_DIR}/src/*.hpp
    ${PROJECT_SOURCE_DIR}/src/*.cpp)

# ------------------------------------------------------------------------------
# Verdict
# ------------------------------------------------------------------------------
if (violations)
    message(FATAL_ERROR "Architecture guard violations:${violations}\n")
endif ()

message(STATUS "Architecture guard: OK (domain/application/plugin/SDK/layering/concurrency/token/TLS checks passed)")
