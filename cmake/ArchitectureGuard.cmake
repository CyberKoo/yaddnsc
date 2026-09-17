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
guard_check("domain must not include http_client/Glaze/spdlog/CLI11/CancellationToken"
    "${INC_RE}[<\"](http_client/|glaze/|spdlog/|CLI/|[^\">]*[Cc]ancellation[Tt]oken[^\">]*)"
    ${PROJECT_SOURCE_DIR}/src/domain/*.h
    ${PROJECT_SOURCE_DIR}/src/domain/*.hpp
    ${PROJECT_SOURCE_DIR}/src/domain/*.cpp)

# ------------------------------------------------------------------------------
# 2. application: no infrastructure implementation headers or third-party I/O
# ------------------------------------------------------------------------------
guard_check("application must not include infrastructure/spdlog/Glaze/CLI11/OpenSSL/dlopen"
    "${INC_RE}[<\"](infrastructure/|core/|composition/|cli/|spdlog/|glaze/|CLI/|openssl/|dlfcn\\.h)"
    ${PROJECT_SOURCE_DIR}/src/application/*.h
    ${PROJECT_SOURCE_DIR}/src/application/*.hpp
    ${PROJECT_SOURCE_DIR}/src/application/*.cpp)

# ------------------------------------------------------------------------------
# 3. plugins: SDK only — no host src/ modules, no legacy utility headers,
#    no spdlog (logging goes through Host Services)
# ------------------------------------------------------------------------------
guard_check("plugins must not include host src/ module headers"
    "${INC_RE}\"(\\.\\./|(core|application|infrastructure|domain|config|network|http_client|cli|composition|dns|ip_source|util)/)"
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
file(STRINGS ${ABI_HEADER} abi_includes REGEX "${INC_RE}<")
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
        set(violations "${violations}\n  ${f}: no parallel HTTP types (use include/http/types.h + interface/http_client.h)\n      ${stripped}")
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
    ${PROJECT_SOURCE_DIR}/plugin_crypto/CMakeLists.txt)
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
# Verdict
# ------------------------------------------------------------------------------
if (violations)
    message(FATAL_ERROR "Architecture guard violations:${violations}\n")
endif ()

message(STATUS "Architecture guard: OK (domain/application/plugin/SDK/boundary checks passed)")
