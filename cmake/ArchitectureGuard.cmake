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
#    Only the listed public coroutine headers are allowed. Template headers may
#    include runtime internals themselves; application code cannot name them.
# ------------------------------------------------------------------------------
set(application_coro_headers
    async_mutex.hpp cancel_scope.h cancelled.h checkpoint.hpp fwd.h group.hpp mutex_guard.hpp now.hpp
    offload.hpp scope.hpp scope_outcome.hpp signal.hpp sleep.hpp task.hpp task_group.hpp time.h)
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
        if (line MATCHES "${INC_RE}[<\"]infrastructure/coro/([^\">]+)[\">]")
            list(FIND application_coro_headers "${CMAKE_MATCH_1}" public_header_index)
            if (NOT public_header_index EQUAL -1)
                continue()
            endif ()
        endif ()
        string(STRIP "${line}" stripped)
        set(violations "${violations}\n  ${f}: application may include only public coro headers, not infrastructure/spdlog/Glaze/CLI11/OpenSSL/dlopen\n      ${stripped}")
    endforeach ()
endforeach()

# Coroutine symbol boundary: normalize simple aliases to coro, then reject
# implementation types and namespace imports. Scope.cancel()/cancelled() and
# unrelated objects' release()/parent() methods remain valid.
foreach (f ${application_check_files})
    file(READ ${PROJECT_SOURCE_DIR}/${f} application_code)
    # Blank out string literals *before* stripping comments. "//" inside a URL
    # literal is not a comment: treating it as one deletes the remainder of the
    # line and silently disables every symbol check on that line.
    string(REGEX REPLACE "\"[^\"\n]*\"" "\"\"" application_code "${application_code}")
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " application_code "${application_code}")
    string(REGEX REPLACE "//[^\n]*" " " application_code "${application_code}")
    string(REGEX REPLACE "[ \t\r\n]+" " " application_code "${application_code}")
    string(REGEX REPLACE "[ ]*::[ ]*" "::" application_code "${application_code}")

    # Iterate to handle aliases of aliases regardless of declaration order.
    set(alias_changed TRUE)
    while (alias_changed)
        set(alias_changed FALSE)
        if (application_code MATCHES "namespace ([A-Za-z_][A-Za-z_0-9]*) *= *(::)?coro *;")
            set(coro_alias "${CMAKE_MATCH_1}")
            string(REGEX REPLACE "namespace ${coro_alias} *= *(::)?coro *;" " " application_code "${application_code}")
            string(REGEX REPLACE "namespace ([A-Za-z_][A-Za-z_0-9]*) *= *(::)?${coro_alias} *;" "namespace \\1 = coro;" application_code "${application_code}")
            string(REGEX REPLACE "(^|[^A-Za-z_0-9])${coro_alias}::" "\\1coro::" application_code "${application_code}")
            set(alias_changed TRUE)
        endif ()
    endwhile ()

    set(coro_internal_types "detail|Context|GetContext|PromiseBase|TaskPromise|TaskAwaiter|FinalSuspend|TaskAccess|WaitNode|TimerNode|CancelCause|LoopAccess|ScopeAccess|GroupState|ChildSlot|Loop|Clock|SystemClock|ManualClock|FdAwaitable|FdToken|ResultBox")
    if (application_code MATCHES "(^|[^A-Za-z_0-9])coro::(${coro_internal_types})([^A-Za-z_0-9]|$)"
        OR application_code MATCHES "using namespace (::)?coro([ ;:]|$)"
        OR application_code MATCHES "namespace [A-Za-z_][A-Za-z_0-9]* *= *(::)?coro::"
        OR application_code MATCHES "::promise_type([^A-Za-z_0-9]|$)")
        set(violations "${violations}\n  ${f}: application must use public coro APIs, not runtime types/context or namespace imports")
    endif ()

    # Loop scheduling/registration and CancelScope waiter bookkeeping are private
    # members reached through coro::detail::LoopAccess / ScopeAccess. cancel()
    # and throw_if_cancelled() are public scope operations and stay allowed.
    # These names also cover unrelated objects' release()/parent() methods.
    if (application_code MATCHES "(\\.|->|::) *(add_waiter|remove_waiter|timeout_action|bind_context|absorbs)([^A-Za-z_0-9]|$)")
        set(violations "${violations}\n  ${f}: application must not call coroutine waiter/context management methods")
    endif ()
endforeach ()

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
# 5. no parallel HTTP types (net::http types + the HttpClient port are the
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
        set(violations "${violations}\n  ${f}: no parallel HTTP types (use src/infrastructure/http/types.h)\n      ${stripped}")
    endforeach ()
endforeach ()

# ------------------------------------------------------------------------------
# 6. host code must not cross the plugin boundary with the plugin-side C++
#    helper (the host speaks driver_abi.h only)
# ------------------------------------------------------------------------------
guard_check("host code must not include the plugin-side SDK helper driver.hpp"
    "${INC_RE}[<\"]yaddnsc/sdk/driver\\.hpp"
    ${PROJECT_SOURCE_DIR}/src/*.h
    ${PROJECT_SOURCE_DIR}/src/*.hpp
    ${PROJECT_SOURCE_DIR}/src/*.cpp)

# ------------------------------------------------------------------------------
# 7. no -rdynamic / export-dynamic backfill: plugins must not resolve host
#    symbols (comment lines are ignored)
#
#    ENABLE_EXPORTS is deliberately *not* matched here. It is an ordinary CMake
#    target property that test fixtures and shared-library targets legitimately
#    set, and this textual check cannot tell a plugin host from a test binary.
#    Exporting symbols from the production executable stays a review item (see
#    cmake/Executable.cmake), not a name ban.
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
    file(STRINGS ${PROJECT_SOURCE_DIR}/${f} lines REGEX "rdynamic|export-dynamic|dynamic_lookup")
    foreach (line ${lines})
        if (NOT line MATCHES "^[ \t]*#")
            string(STRIP "${line}" stripped)
            set(violations "${violations}\n  ${f}: plugins must not resolve host symbols (no -rdynamic/export-dynamic)\n      ${stripped}")
        endif ()
    endforeach ()
endforeach ()

# ------------------------------------------------------------------------------
# 8. dns_classic (wire format, parser, validator, resolv.conf) stays below the
#     HTTP/transport layers: it must not pull in OpenSSL or the plugin tree. The
#     coroutine resolver facades (dns/resolver/) are the layer above and are checked
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
# 9. Concurrency is an implementation detail of the coroutine runtime. A direct
#     <thread>, <future> or BS::thread_pool include is allowed only in the
#     runtime (src/infrastructure/coro/) and in the single place that must hand
#     a result back across the synchronous plugin ABI boundary
#     (src/infrastructure/plugin/bridge.*). Everywhere else in the
#     application / infrastructure / composition layers, route the work through
#     coro::offload or a coro::Task. The plugin boundary (driver/) is a separate
#     binary boundary and is deliberately not checked; src/support/ is a generic
#     utility layer and is also not checked.
#
#     Scope: this is an include check, so it sees <thread>/<future>/BS_thread_pool
#     only. It cannot see a raw pthread_create()/clone(), and <pthread.h> is
#     legitimately included for pthread_sigmask (net/tls_stream.cpp). C-level
#     thread creation therefore stays a review item under the Concurrency rule
#     in rules/04, not something this check proves.
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
# 10. Regression guard for a deleted design. The legacy per-operation
#     cancellation token is gone: cancellation is scope state reached through
#     checkpoints (see the Concurrency rule in rules/04). The two removed type
#     names and the removed header must not reappear under src/.
#
#     Matched on whole identifiers only, so unrelated names that merely contain
#     the substring (a `cancellation_source_count` local, a config key) are not
#     flagged. Full-line comments are already exempt; keep free-standing notes
#     about the migration on their own comment line.
# ------------------------------------------------------------------------------
guard_check("the legacy cancellation token must not reappear (cancellation is scope state)"
    "(^|[^A-Za-z_0-9])Cancellation(Token|Source)([^A-Za-z_0-9]|$)|cancellation_token\\.hpp"
    ${PROJECT_SOURCE_DIR}/src/*.h
    ${PROJECT_SOURCE_DIR}/src/*.hpp
    ${PROJECT_SOURCE_DIR}/src/*.cpp)

# ------------------------------------------------------------------------------
# 11. rules/ states discipline, docs/ states project facts. A normative rule
#     that names a project source path or a project symbol has to be edited
#     every time that path or symbol is renamed, so the fact belongs in an
#     owner document and the rule states the principle plus a link instead.
#
#     See "Documentation Maintenance" in rules/04. Scope and limits:
#       - rules/05-examples.md is excluded: it is explicitly non-normative and
#         pointing at real files is its purpose.
#     - path references must be backtick-quoted, which is how rules/ cites them;
#       prose like "Build/test/CI" is not a path. Links to docs/, AGENTS.md and
#         the guard's own test name are fine: those defer to an owner document
#         rather than restate a fact.
#       - CMake's regex engine has no \b, so identifier edges are spelled out.
#       - this is a textual check over paths and qualified symbols. A rule that
#         names a project fact in prose without a path or a `ns::symbol` still
#         needs review; the check does not prove that rules/ is fact-free.
# ------------------------------------------------------------------------------
set(rules_check_files
    01-language-and-build.md 02-implementation.md 03-error-handling.md
    04-quality-and-process.md)
string(CONCAT rules_project_ref
    "\`((src|include/yaddnsc|driver|test|cmake)/[A-Za-z0-9_./-]*)\`"
    "|((^|[^A-Za-z_0-9])(coro|app|domain|net|dns|configuration)::[A-Za-z_])"
    "|((^|[^A-Za-z_0-9])(YLOG|SPDLOG|YADDNSC)_[A-Z_])"
    "|((^|[^A-Za-z0-9_])[A-Z][A-Z_]*_BUDGET([^A-Za-z0-9_]|$))")
foreach (rf ${rules_check_files})
    if (NOT EXISTS ${PROJECT_SOURCE_DIR}/rules/${rf})
        continue()
    endif ()
    file(STRINGS ${PROJECT_SOURCE_DIR}/rules/${rf} lines REGEX "${rules_project_ref}")
    foreach (line ${lines})
        string(STRIP "${line}" stripped)
        set(violations "${violations}\n  rules/${rf}: a normative rule must not name a project path or symbol; state the principle and link the owner document\n      ${stripped}")
    endforeach ()
endforeach ()

# ------------------------------------------------------------------------------
# Verdict
# ------------------------------------------------------------------------------
if (violations)
    message(FATAL_ERROR "Architecture guard violations:${violations}\n")
endif ()

message(STATUS "Architecture guard: OK (domain/application/plugin/SDK/layering/concurrency/token/rules checks passed)")
