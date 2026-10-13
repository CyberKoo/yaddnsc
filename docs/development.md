# Development

This document records development commands and current build/CI behavior.
Contributor requirements live in [rules/](../rules/), including
[Language, Compiler & Build](../rules/01-language-and-build.md); this is not a
second rule set. It is not required for normal yaddnsc operation.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

Use `-DCMAKE_BUILD_TYPE=Release` for production builds.

Trace-level logging (`SPDLOG_TRACE`, selectable at runtime via
`yaddnsc run --log-level=trace`) is compiled out by default; compile it in
with `-DYADDNSC_ENABLE_TRACE=ON`.

Supported current toolchains are CMake 3.28+, a 64-bit target, C++23-capable
GCC 14+, Clang 19+, or Apple Clang 15+, and OpenSSL 3.0+.

### Build options

The cache variables below are defined in `cmake/ProjectSetup.cmake` and
`cmake/Packaging.cmake`; `YADDNSC_BUILD_DOCS` and `YADDNSC_IWYU` are defined
beside the features they gate (see [Documentation](#documentation) and
[Include hygiene](#include-hygiene) below).

- `YADDNSC_BUILD_TESTS` (default `OFF`) adds `test/` and enables CTest;
- `YADDNSC_DEVELOPMENT` (default `ON`) embeds `git describe` version info in
  the binary; `OFF` forces `RELEASE`;
- `YADDNSC_SANITIZE_DEBUG` (default `ON`) adds ASan/UBSan in `Debug`;
- `YADDNSC_ENABLE_TRACE` (default `OFF`) compiles trace-level logging in;
- `YADDNSC_USE_SYSTEM_SPDLOG` (default `OFF`) uses the system spdlog instead of
  the bundled CPM version; with spdlog < 1.15 (e.g. Ubuntu 24.04's 1.12) the
  async log queue falls back from the `discard_new` to the `overrun_oldest`
  overflow policy;
- `YADDNSC_LOGGING_PATTERN` (default
  `"[%D %T.%e] [%^%8l%$] [%8!t] [%15!s:%-4#] %v"`) is passed to
  `spdlog::set_pattern()`;
- `YADDNSC_MIN_UPDATE_INTERVAL` (default `60`) is the minimum allowed update
  interval in seconds; a negative value fails the configure;
- `YADDNSC_DEFAULT_DNS_SERVER` (default `1.1.1.1`) and
  `YADDNSC_DEFAULT_DNS_PORT` (default `53`) are the fallback resolver used when
  no nameserver is configured; the address is validated as an IPv4/IPv6 literal;
- `YADDNSC_ENABLE_DEB` (default `OFF`) enables CPack DEB generation, producing
  two packages: `yaddnsc` (binary, drivers, config, systemd unit, shell
  completions) and `yaddnsc-dev` (plugin SDK headers, CMake package config,
  header-only signing helpers and bundled Glaze).

### Build layout

The root `CMakeLists.txt` orchestrates the build. Edit the module under `cmake/`
that owns the relevant concern; keep the root file focused on loading order:

1. `ProjectSetup.cmake` — C++ compilation environment, feature detection,
   user options, version information, sanitizers, and `GNUInstallDirs`.
2. `GeneratedHeaders.cmake` — all six `configure_file` calls, after setup
   has defined their input variables.
3. `Dependencies.cmake` — third-party dependencies and wrapper targets.
   GoogleTest and Google Benchmark remain in `test/`.
4. `BuildOptions.cmake` — fmt, warnings, build_options, and internal_headers
   interface targets, the production-module helper, and first-party IWYU
   activation after third-party targets have been created.
5. `ProductionTargets.cmake` — all production modules, explicit source lists,
   and their dependency graph, using the shared interfaces and helper.
6. `Executable.cmake` — main executable linking, sanitizer application,
   Release stripping, and IPO.
7. `PluginSdk.cmake` — SDK/XML/crypto handling and C ABI checks, which are built
   regardless of `YADDNSC_BUILD_TESTS`.

Keep the configuration summary and Doxygen setup between `Executable.cmake`
and `PluginSdk.cmake`. After the SDK, retain this order: `driver/`, `HeaderCheck.cmake`, `test/`, then `InstallRules.cmake`, preserving
existing conditions. The SDK must be available before its consumers.

Use `include()` for these modules to preserve the root directory context;
do not add `CMakeLists.txt` files under `src/`. This is an organizational
refactor only: build commands, targets, and dependency versions are unchanged.

## Tests

```bash
cmake -S . -B build-tests \
  -DCMAKE_BUILD_TYPE=Debug \
  -DYADDNSC_BUILD_TESTS=ON
cmake --build build-tests --parallel
ctest --test-dir build-tests --output-on-failure
```

Two convenience targets avoid building everything first: `drivers` builds the
bundled driver plugins, and `tests` builds every test executable plus
`yaddnsc` and the drivers:

```bash
cmake --build build-tests --target drivers
cmake --build build-tests --target tests
```

`integration_scenarios` hard-requires the `yaddnsc` binary and the `simple`
driver `.so` to be built already and fails with a clear message otherwise.
The `bench_*` targets are `EXCLUDE_FROM_ALL`, so benchmarks are built only when
asked for (`cmake --build build-tests --target bench_dns_parser`).

Tests use GoogleTest/GoogleMock as the project's fixed framework pair; a test
names its case `TEST(ClassName, MethodName_Scenario_ExpectedBehavior)`.
Component tests may use loopback sockets, local helper processes, or platform
facilities; they do not require provider credentials or external DNS provider
access.

### Test tiers

| Tier        | Location                             | Runs the real binary?                 |
| ----------- | ------------------------------------ | ------------------------------------- |
| Unit        | `test/unit/`                         | No — no external network or real provider; loopback sockets and temp files do occur |
| Driver      | `test/driver/`                       | No — each driver is compiled into its test binary and driven through the C ABI with fake host services |
| Component   | `test/component/`                    | Real loopback sockets, multicast, TLS |
| Plugin ABI  | `test/plugin/`, `test/sdk_consumer/` | Loads a real `.so` via dlopen         |
| Integration | `test/integration/`                  | Yes — the built `yaddnsc` binary      |
| Benchmarks  | `test/perf/`                         | No — Google Benchmark                 |

The driver list behind `test/driver/` comes from `cmake/Drivers.cmake`, the
same source `driver/` builds from, so a new bundled driver gets its test
target automatically — except for libxml2-based drivers such as `namecheap`
and `route53`, whose test targets are skipped when libxml2 was not found.

`test/sdk_consumer/` is the exception in the table: it is not part of the
CTest tree but a standalone CMake project that builds against the *installed*
package (see below).

Within `test/unit/`, the layout mirrors the `src/` layering: `domain/`,
`application/`, `cli/`, `coro/`, `infrastructure/<module>/` and `support/`
hold the tests of their namesake source layer; `src/composition/` has no unit
counterpart. Two extra directories have no `src/` counterpart: `sdk/` covers
the public plugin headers, and `plugin_support/` tests the SDK signing helpers.
CTest target names are stable across moves; only file paths follow the source
tree.

The component tier's wire-protocol helpers live in `test/component/servers/`:
small Python DNS/DoH/DoT/TLS echo servers that the test binaries start on
loopback ports. They answer single protocol exchanges for in-process clients
— unlike `test/integration/sim/server.py`, which emulates a complete provider
plus DNS for the real `yaddnsc` binary end to end. When one of those servers
cannot start, the affected component cases call `GTEST_SKIP()` instead of
failing, so a green component run is not by itself proof they executed.

### Integration scenarios

`test/integration/run_ctest.sh` drives the real binary against a Python DNS
simulator (`test/integration/sim/server.py`, aiohttp + dnslib). The tier is
registered only when bash and Python3 are found at configure time (bash is a
ports/pkg install on FreeBSD, not part of the base system). It covers
three end-to-end update paths (interface/classic, http/DoT, interface/DoH),
the read-only CLI subcommands and their exit codes, two negative `config test`
checks (a malformed config and a missing file must both be reported), a run
against a malformed config that must abort instead of looping on it, and two
failure contracts that pull in opposite directions:

- an HTTP IP source returning a non-address response → the update workflow
  reports no usable IP address and nothing is published;
- no DNS answer → the update is still published, because
  the `update_task` implementation treats an unverifiable current record as
  an empty one. Pushing an unchanged record is harmless; skipping a changed
  one is not.

Run just this tier:

```bash
ctest --test-dir build-tests -R integration_scenarios --output-on-failure
```

**Check for skipped execution.** `integration_scenarios` carries
`SKIP_RETURN_CODE 77`, so CTest reports the test as skipped when
`python3 -m venv` cannot bootstrap pip (`python3-venv` missing). A successful
CTest exit does not prove the scenarios executed — use verbose output
(`ctest -V -R integration_scenarios`) and check for `ALL SCENARIOS PASSED`.
Negative update scenarios require the expected workflow diagnostic and a
successful shutdown; simulator reset/log failures are test failures, not
evidence that no update was sent. The mDNS group users (`test_mdns_coro`,
`integration_scenarios`) hold the `mdns-multicast` `RESOURCE_LOCK` and must not
be run concurrently outside CTest.

### Coroutine cancellation regressions

The runtime suite covers reusable mutex/sleep operations, cancellable Handle
reservations and offload admission/capture release. `test_net_coro_unit` covers
fd-operation reuse, while runtime tests exercise concurrent lock descriptors,
Handle reservation conflicts, FIFO admission and capacity held through running
cancellation. `test_coro_allocation_failure` checks pool initialization and both
immediate/deferred submission failures, as well as mutex queue growth failure.
`test_plugin_cancellation_publication` uses an isolated allocator rendezvous to
cancel after the Host Services entry check and before request copying completes.
It also injects worker post, loop admission and real gateway request-copy failures.
`test_ip_source_offload` interposes `getifaddrs` in its own executable: a blocked
metadata read must allow a loop timer to cancel the caller, and source destruction
must not invalidate the remaining worker packet. Its socket interposition also
proves default IPv4 mDNS proceeds to socket setup while the pool is saturated,
and checks interface filtering, IPv6 scope IDs and structured SIGTERM cancellation.
Gateway tests verify same-thread ABI cycles, host cancellation of validation,
overlapping retiring HTTP requests, and cancellation after result publication.
Host defects reach active callers or the root error boundary after abandonment.
The certificate suite snapshots multiple issuers and AUX, rejects misnamed roots,
and fixes the fail-closed policy for malformed entries, directories and dangling
links alongside valid roots. `test_trust_fallback` runs the real context factory
in fresh processes with isolated discovery/default-directory adapters, covering
directory-only trust and explicit-bundle precedence. No production test hooks
are added. The real TLS component verifies a peer after snapshot source removal.

`bench_coro_offload` includes `BM_metadata_snapshot/0` (synchronous lookup baseline)
and `/1` (offload round trip). Run benchmarks in Release without sanitizers;
these compare cost, not a guaranteed kernel-call latency or throughput improvement.
Existing loopback, TLS and multicast test environment requirements still apply.

### Installed SDK consumer validation

CTest's `sdk_install_consumer` installs the `dev` and `glaze_Development`
components into a fresh per-invocation prefix (concurrent CTest runs are isolated), copies the installed examples outside the
checkout, builds real C11/C++ HTTP/XML/crypto modules and loads them through
`dlopen` for complete ABI cycles. Crypto uses multiple translation units; GCC
also tests the alternate libstdc++ ABI. Consumers are built without host
sanitizers or source/CPM include paths. Checks cover no imposed warning/visibility
policy, exact Glaze selection/failure diagnostics, missing optional dependencies,
pure C without Glaze/OpenSSL, and install-prefix relocation.

```bash
ctest --test-dir build-tests --output-on-failure -R sdk_install_consumer
# Or check an already installed development component/package:
cmake -DSDK_PREFIX=/tmp/yaddnsc-sdk -DWORK_DIR=/tmp/sdk-check \
  -DVERIFY_OLD_CXX_ABI=ON -P test/sdk_consumer/verify.cmake
```

The development package requires exactly Glaze v7.8.2 and includes its metadata;
`cmake --install --component dev` alone is incomplete for C++ JSON consumers:
install `glaze_Development` into the same prefix too. XML is an optional component
available only when the host was configured with libxml2. Crypto is header-only
in `include/yaddnsc/sdk/signing.hpp` and requires OpenSSL 3.0+.

The `sdk-package` CI job configures a Release package, generates the final
`yaddnsc-dev` DEB (no host build is needed for header-only development contents),
installs it and its declared dependencies in a fresh Ubuntu container, and runs
the installed verification script with no host package or source checkout.
The final runtime DEB is built by release workflows separately.

### Local CI simulation

`test/ci-sim/` builds and runs the project inside Ubuntu 24.04 containers —
Alpine/musl only under `--musl` — and is a **local** tool for reproducing the
CI environment; it is not invoked by any workflow.

```bash
./test/ci-sim/run.sh          # linux-amd64
./test/ci-sim/run.sh --musl   # linux-amd64-musl
./test/ci-sim/run.sh --all    # both
```

## Coverage

The nightly coverage job uses GCC instrumentation, runs the CTest suite, and
uploads an LCOV report to Codecov. For a local report, use GCC 14, an installed
`fastcov`, and a fresh build directory:

```bash
CC=gcc-14 CXX=g++-14 cmake -S . -B build-coverage \
  -DCMAKE_BUILD_TYPE=Debug -DYADDNSC_SANITIZE_DEBUG=OFF \
  -DYADDNSC_BUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS="--coverage -O0 -fprofile-update=atomic" \
  -DCMAKE_EXE_LINKER_FLAGS="--coverage -fprofile-update=atomic"
cmake --build build-coverage --parallel
ctest --test-dir build-coverage --output-on-failure
fastcov -g gcov-14 -d build-coverage \
  -i "src/" "include/" "driver/" \
  -e "test/" "_deps/" "/usr/" "*/generated/" \
  -B -o coverage.info --lcov
```

`-B` keeps all branch records. It matters here because GCC's coroutine split
clones each coroutine body into `.actor`/`.destroy` functions with their own
counter arrays, and gcov's line table collapses those clones away — coroutine
bodies largely disappear from the *line* report (e.g. a 200-line task
coroutine may report only its ramp entry lines), so line coverage is
optimistically skewed on coroutine-heavy files while missing exactly the
cancellation / timeout / redirect paths. Branch records survive the collapse
and are the reliable signal for that code; fastcov's `-b` (useful branches
only) filters most of them out, so use `-B`. Read the branch column of the
lcov summary alongside the line column.

Even the branch numbers have a hard ceiling: gcov reports only the *ramp*
function's branch table, and much of it is coroutine-lowering plumbing
(state-machine dispatch, frame EH edges, `bad_alloc` unwinds) that no test can
reach — several coroutine TUs sit at their proven artifact ceiling (e.g.
`update_task.cpp` 33%, `transport.cpp` 32%) with every behavioural branch
already covered. Chase behaviour, not the number.

### Coverage gate

`test/coverage_gate.py` enforces a two-track gate on the tracefile (the
nightly coverage job runs it before the Codecov upload):

```bash
python3 test/coverage_gate.py coverage.info            # add -v to list tracks
```

- A TU whose source contains `co_await` / `co_return` / `co_yield` is a
  *coroutine TU*: its line data is collapsed as described above, so it is
  gated on **branch coverage** (floor: 48%).
- Every other TU is gated on **line coverage** (floor: 80%).

A line-only tracefile is rejected, so the gate cannot pass silently without
branch data. Both floors are ratchets — raise them with `--line-min` /
`--branch-min` (and then the defaults) as coverage improves; never lower
them. 80% remains the line-coverage target. A Codecov upload error can also
fail the job.

## Sanitizers

Targets using `add_sanitizer_flags()` receive ASan/UBSan in Debug by default.
The separate `Sanitizer` build type adds use-after-scope instrumentation and
conditionally adds the integer, bounds, null, and alignment checks plus
use-after-return, each according to configure-time compiler probes. The core
ASan/UBSan flags still require toolchain and runtime support, so the result is
not a guaranteed full combination on every platform. Release does not receive
these sanitizer flags.

Run the Debug sanitizer tests with:

```bash
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DYADDNSC_SANITIZE_DEBUG=ON \
  -DYADDNSC_BUILD_TESTS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

For the dedicated configuration, replace `Debug` with `Sanitizer` and use a
separate build directory. The nightly sanitizer job also sets:

```bash
export ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2
```

Coverage uses `YADDNSC_SANITIZE_DEBUG=OFF` to avoid mixing instrumentation.

### Coroutine regression tests

```bash
# Linux: omit test_coro_syscall_failure on other platforms.
cmake --build build-tests --target test_coro test_coro_allocation_failure test_coro_syscall_failure test_stop_signals --parallel
ctest --test-dir build-tests -R '^(test_coro(_allocation_failure|_syscall_failure)?|test_stop_signals)$' --output-on-failure
```

The allocation-failure binary replaces allocation functions only inside that
executable. Its thread-local one-shot faults check failed group-slot allocation
and timer-heap growth without adding production test hooks. It also verifies
that scope registration, removal and tree traversal do not consume an injected
allocation failure. Process stop tests
cover early arrivals, fd wakeup, repeated Ctrl-C, handler restoration and
cancellation of already-latched waits. The Linux-only syscall-failure binary
uses linker wrapping to check interrupted/full/failed wake writes and
interrupted/failed poll dispatch without production hooks.
The core binary also checks deferred callable ownership, throwing result moves,
single-consumer joins, typed completion exhaustion, caller-scope result-wait
cancellation (including completion/timeout ties), consumption-time frame recovery,
unobserved supervisor failures, exception-safe body/resource drains, joined and
cooperative worker cancellation, inbox pumping after root exit, loop-thread-only
trace callbacks and the fatal loop-dispatch policy. Regressions also cover
nested manual cancellation draining group children, Handle claims waking typed
result waiters, empty Task awaits, closed nonnegative fds versus hangup readiness,
nonassignable scope results, lvalue-qualified offload callables and saturated
time arithmetic at representation limits. Cancellation-tree tests cover sibling
removal and reinsertion, const observer scopes, depth-first/waiter ordering,
shielding, subtree boundaries and fatal destruction with live children. A
100,000-scope chain is cancelled in a child process with at most 1 MiB of native
stack, so an unlimited host stack cannot hide recursive traversal regressions.
Plugin tests cover closed-group admission, worker-deadline bridge timeout,
remaining-budget translation to a manual loop clock with fake transports, and
cancellation before the exchange scope is published. HTTP session tests cover
loop-clock keep-alive expiry and cancellation of stalled writes/header/body
reads without retaining the stream or session mutex. The isolated
`test_random_failure` executable replaces RAND_bytes to verify checked entropy
failure, explicit-ID construction without entropy and operational resolver
translation. It also drives Dispatcher with one or three real backends under
all three strategies: preflight acquires no entropy, backend CONNECTION results
receive the configured recovery attempt, and invalid names are rejected before
entropy acquisition. It does not change OpenSSL configuration in other test
processes.
mDNS receive tests use isolated loopback sockets to verify mixed discard bursts,
backoff, the unchanged caller window and cancellation during backoff. Debug sanitizer builds exercise these same
tests; a passing suite does not substitute for separate TSan/platform validation.

## Documentation

Doxygen is optional:

```bash
cmake -S . -B build-docs \
  -DCMAKE_BUILD_TYPE=Release \
  -DYADDNSC_BUILD_DOCS=ON
cmake --build build-docs --target doxygen
```

## Dependency maintenance

Bundled dependencies use CPM.cmake to integrate upstream CMake projects; some
libraries also have system-package paths. `cmake/CPM.cmake` currently downloads
CPM 0.43.1 with a SHA-256 check on the bootstrap download. Dependency declarations
mostly select release tags, which upstream can move; this bootstrap hash does
not make dependency tags immutable. A full Git commit SHA identifies a specific
source revision. CPM's source cache is not a binary cache, and the setup provides
neither a general transitive-version resolver nor automatic vulnerability
scanning.

## Plugin ABI changes

The v1 plugin ABI (`include/yaddnsc/sdk/driver_abi.h`) is **stable since ABI
1.0** — it is the only stable surface across the `.so` boundary, and the
rules in its header comment are the contract. The C++ helper layer
(`include/yaddnsc/sdk/driver.hpp`) is source-level and may evolve freely as
long as the entry points it emits keep conforming to the C ABI.

### Bump classification

Minor bump (`YADDNSC_DRIVER_ABI_MINOR` + 1):

- appending a field to one of the versioned top-level structs
  (`yaddnsc_error`, `yaddnsc_http_request`, `yaddnsc_http_response`,
  `yaddnsc_update_request`, `yaddnsc_driver_descriptor`,
  `yaddnsc_host_services`);
- adding a host-services callback, capability bit, status code, log level,
  or HTTP method.

Major bump (`YADDNSC_DRIVER_ABI_MAJOR` + 1 — a new ABI; open a design
discussion first):

- moving, inserting, or removing a field, or growing a leaf type
  (`yaddnsc_string`, `yaddnsc_bytes`, `yaddnsc_source_location`,
  `yaddnsc_http_header`);
- changing a pinned constant value, an entry-point signature, or the
  semantics of an existing field;
- adding a required entry point.

### Minor-bump checklist

Every ABI minor bump must:

1. append new fields at the END of the struct — existing offsets never move
   (note: inserting into tail padding changes no offset, and is still
   forbidden);
2. read every post-1.0 field behind `yaddnsc_struct_has_field`;
3. pin new constant values in the assertion block of `driver_abi.h`, and
   update the `YADDNSC_DRIVER_ABI_MINOR` pin there;
4. keep the `YADDNSC_*_MIN_SIZE` macros pointed at the ABI 1.0 baseline —
   never retarget them;
5. keep `test_abi_freeze_compat` green: the frozen v1.0 plugin under
   `test/plugin/abi_baseline/` compiles against the frozen 1.0 header copy
   and must keep loading and running unchanged;
6. never edit the frozen copy — CI verifies its sha256
   (`test/plugin/abi_baseline/v1_0/driver_abi.h.sha256`).

The compile-time pins in `driver_abi.h` (constant values, field offsets,
baselines, leaf sizes, alignment) run in every build on every target, so a
violation fails compilation before any test runs.

## CI and warning gates

End-user build instructions live in the README. The workflows currently cover
the following jobs; their existence does not establish branch-protection rules
or an all-platforms-before-merge requirement. Markdown-only changes are excluded
by `ci.yml` path filters.

`ci.yml` (push/PR):

- `linux-amd64` — GCC Debug + full test suite (includes the plugin contract
  tests and the `architecture_guard` boundary checks);
- `linux-amd64-musl`, `macos-arm64` — platform matrix (glibc/musl/macOS);
- `conversion-gate` (PR only) — `-Wconversion -Wsign-conversion` build;
- `benchmark` (PR only) — Google Benchmark smoke run, with no stored historical
  baseline, comparison step, or performance-regression threshold;
- `plugin-contract` — builds the whiteboard test plugin and runs the dlopen /
  Host Services ABI contract tests explicitly; also verifies the frozen ABI 1.0
  baseline header by sha256 and runs the frozen-plugin compatibility test
  (`test_abi_freeze_compat`).

`nightly.yml`:

- Linux arm64 Debug tests, Release builds with bundled/system spdlog (tests
  disabled), the dedicated `Sanitizer` build/tests, and coverage (with the
  two-track coverage gate, see "Coverage" above);
- `linux-amd64-clang` is **disabled** (`if: false`). Upstream Clang coverage on
  Linux would come from that job; macOS uses AppleClang.

Scheduled nightly builds run only when the recent-commit check passes; manual
runs bypass that check. Linux arm64 is nightly/manual coverage, not a PR check.

`yaddnsc_warnings` supplies `-Wall -Wextra -Wpedantic -Wshadow -Werror` to
production modules, the executable, header checks, and tests that link the shared
warning interfaces. SDK/C ABI/plugin-crypto targets carry their own warning
options. `BuildOptions.cmake`
also suppresses `maybe-uninitialized` in `Sanitizer` builds and demotes
`array-bounds` for system fmt below version 10. The conversion job adds
`-Wconversion -Wsign-conversion` on top of the defaults above.

## Formatting and static analysis

`.clang-format` is the formatting source of truth. Formatting is applied from
it during development; CI enforces compilation and tests rather than style.

`cmake/ClangTidy.cmake` attaches clang-tidy when the unversioned `clang-tidy`
executable is found and the compiler ID matches Clang (including AppleClang).
The nightly `linux-amd64-clang` job (currently `if: false`) installs the
versioned `clang-tidy-19`, which that unversioned-only lookup would not pick up.
Otherwise analysis is skipped. `.clang-tidy` enables a long `Checks` list of
roughly 150 entries across bugprone, clang-analyzer, cert, cppcoreguidelines,
google, hicpp, misc, modernize, performance, readability and concurrency —
including `bugprone-use-after-move` and `clang-analyzer-cplusplus.InnerPointer`,
but not `readability-identifier-naming` or `clang-analyzer-cplusplus.Move`. It
does not automatically enforce the naming rules. No `WarningsAsErrors` or
`--warnings-as-errors` setting promotes tidy warnings to failures; compiler
`-Werror` is not a clang-tidy diagnostic gate. Tool execution/parsing errors may
still fail the build.

## Include hygiene

- **IWYU** (`cmake/IWYU.cmake`) is enabled by default only when the tool is
  found and the compiler is upstream Clang, not GCC or AppleClang. It uses
  `--error` to fail on violations and `.iwyu-mappings.imp` for mappings.
  Activation occurs after third-party targets are created; generated header-check
  translation units opt out. Disable locally with `-DYADDNSC_IWYU=OFF`.
  Installing Homebrew IWYU in the macOS job does not activate it with AppleClang.
- **Header self-containment**: in non-Release builds, `yaddnsc_header_checks`
  compiles individual headers matching `src/*.{h,hpp}`, `include/*.{h,hpp}`,
  `driver/*/*.{h,hpp}` recursively (including signing under the SDK tree).
  `xml_raii.hpp` is excluded when LibXml2 is unavailable. This is a
  default-build check, not coverage of every header in every configuration.
- **Editor feedback**: `.clangd` sets `UnusedIncludes` and `MissingIncludes`
  to `Strict`; editor diagnostics are not CI gates.

## Manual boundary review

The `architecture_guard` ctest (`cmake/ArchitectureGuard.cmake`) checks textual
boundaries — includes, layering, threads/futures and the removed cancellation
token — but it cannot judge every loop-safety decision. Loop safety itself (no
blocking work on the loop thread, including OpenSSL's lazy trust loaders) is
reviewed by hand; see the
[blocking-operation inventory](architecture.md#blocking-operation-inventory).

Beyond the application-layer checks described below, the same script also
enforces, in `cmake/ArchitectureGuard.cmake`:

- lower layers stay below their public capability: `src/domain/` may not include
  `infrastructure/`, Glaze, spdlog, CLI11 or cancellation-token headers, and
  `src/coro/` may not include spdlog (loop trace output goes out through the
  injected `Loop::TraceSink`);
- `network/transport/` may not include the TLS or factory internals, and
  `http/` may not include concrete `dns/` backends — both take an injected
  capability instead;
- plugins are SDK-only: no host `src/` modules and no legacy utility headers
  (`core_logger*`, `CORE_LOG*`, `xml_raii.h`, `form_encode.h`, `uri.h`,
  `fmt.hpp`, `string_util.hpp`, `HttpClient*`, `spdlog/`), since logging
  travels through Host Services;
- `include/yaddnsc/sdk/driver_abi.h` may include only C standard headers
  (`stddef.h`, `stdint.h`, `stdbool.h`);
- no parallel HTTP types: no `HttpTransport`, `HttpRequest` or `HttpResponse`
  definitions outside the plugin SDK's own ABI DTOs;
- host `src/` code may not include `yaddnsc/sdk/driver.hpp` — the host speaks
  `driver_abi.h` only;
- no `-rdynamic`, `export-dynamic` or `dynamic_lookup` in any CMake file, so
  plugins cannot resolve host symbols (`ENABLE_EXPORTS` is deliberately not
  matched);
- `rules/01`–`rules/04` stay fact-free: a backticked project path, an
  `ns::symbol`, a `YADDNSC_*`/`YLOG_*` constant or a `*_BUDGET` constant in a
  normative rule fails the build, which keeps those documents from restating
  project state.

For `src/application/`, the guard allows direct coroutine includes only from
`async_mutex.hpp`, `cancel_scope.h`, `cancelled.h`, `checkpoint.hpp`, `fwd.h`,
`group.hpp`, `group_error.h`, `mutex_guard.hpp`, `now.hpp`, `offload.hpp`, `scope.hpp`,
`scope_outcome.hpp`, `sleep.hpp`, `task.hpp`, `task_group.hpp` and
`time.h`. These coroutine include and internal-access checks apply only to
`src/application/`; the thread/future include check is wider and globs
`src/application/`, `src/infrastructure/` and `src/composition/` together.
For application code, it rejects explicit internal types/context, `coro::detail`,
coroutine namespace imports, `promise_type` access and waiter/context-management
methods. Simple
namespace aliases (including chains) and split qualifiers are normalized.
String literals are blanked out before comments are stripped, so a `//` inside a
URL does not truncate the rest of the line and hide a violation; block and line
comments are otherwise ignored for these symbol checks. Public scope
cancellation/state reads (`cancel()`, `cancelled()`, `timed_out()`,
`throw_if_cancelled()`) and unrelated `release()` calls remain allowed.
Application symbol checks also reject references, imports, namespace reopening
and simple aliases of infrastructure namespace roots (`net`, `dns`, `http`,
`ipsource`, `Config`, `configuration`, `logging`, `plugin`, `infrastructure` and
`Utils::Cert`), including nested/split qualifiers. An alias is rejected at its
infrastructure target, so an alias chain does not need to be resolved. Keep this
root list in the guard aligned with new infrastructure namespaces.
To catch concrete-type forward declarations, the guard harvests ordinary
`class`/`struct` definitions throughout infrastructure `.h`/`.hpp` headers
(excluding the coroutine tree), then rejects matching application forward
declarations outside brace blocks. For this name check only, innermost brace
blocks are iteratively replaced with markers: declarations in `app`/`domain`
namespace blocks may share infrastructure names, such as `app::Stream` versus
`net::Stream` or `domain::SubdomainConfig` versus `Config::SubdomainConfig`.
Infrastructure namespace checks still inspect the original normalized text,
including references inside business namespace blocks. Thus the old `class NetworkInterfaces;`
dependency in `diagnostics.h` and newly named implementation classes cannot
bypass the include boundary merely by omitting the implementation include.
Definitions, not borrowed forward declarations, populate this inventory;
application/domain declarations and public coroutine APIs remain allowed.

Pure contract headers under `src/application/ports/` (`.h`/`.hpp`, including
subdirectories) have a narrower include boundary: internal includes may target
only `domain/`, `application/ports/` or the public coroutine header whitelist
above. All quoted includes and angle includes using known source-root paths are
checked as internal; bare/relative quoted paths and traversal components are
rejected. Standard-library angle includes remain allowed except `<format>`;
`fmt/...` includes are also rejected. This blocks `support/fmt.hpp` and
`application/log_macros.h` from pulling formatting conveniences back into contracts.
Function-like `#define NAME(...)` declarations are rejected in port headers;
object-like macros, including include guards, remain allowed. This does not
restrict the number of ports per header or legitimate contract aggregation.
Ordinary application headers outside `ports/`, including the macro convenience
layer `application/log_macros.h`, may still use support helpers and formatting.

`test_architecture_guard_cases` runs the actual CMake script on isolated source
fixtures, and `test_coro` checks that raw Task/group/Handle construction, Task
context/frame access, and the loop's scheduling/registration services and the
scope's waiter bookkeeping are unavailable to callers.

These are textual checks: they do not resolve C++ types, all macros/aliases,
relative or macro-generated includes, string literals, or arbitrary template
metaprogramming. The concrete-type inventory is a conservative name heuristic,
not scope resolution: it also collects namespaced/nested definitions. Namespace
checks likewise match names without resolving ownership; an unrelated nested
namespace with a reserved infrastructure root name can be rejected.
Macro/attribute-generated or otherwise nonordinary declarations, and
implementation types exposed only through transitive includes or deduction still
require review. Port include/macro checks are line-based: they do not expand
macros or backslash-spliced directives, trace transitive includes, or fully lex
multiline comments; review must check those forms too. The brace-block reduction
is not C++ scope analysis: declarations inside any brace block are excluded from
the inventory name check, and unmatched/macro-generated braces or nonordinary
qualified declarations require review. A same-name declaration outside brace
blocks is still conservatively rejected without proving infrastructure ownership;
this policy does not ban same-name `app`/`domain` declarations in namespace blocks.
Comment/string normalization is not a complete C++
lexer (notably escaped quotes and raw strings), and inactive preprocessor branches
are still scanned. Public headers necessarily include implementation definitions;
review must still check inferred objects and callbacks for runtime access.
The normative boundary is in
[Architecture](architecture.md#coroutine-api-boundary).

When touching startup, transport or IP-source code, confirm by hand:

- **Runtime interface metadata reads leave the loop.** Interface and mDNS
  sources read live snapshots and name/index metadata through owned
  `coro::offload` packets, sharing admission with plugin workers. The enclosing
  timeout covers admission and reading; default IPv4 mDNS uses INADDR_ANY
  without metadata or worker admission. Startup/CLI listings stay synchronous.
  Do not add a cache, mutex or single-flight wait. Keep blocked-provider and
  pool-saturation regressions, and measure loop responsiveness as well as lookup
  wall time when changing this boundary.
- **A CA bundle is only ever loaded off the loop.** Build trust material with
  `net::TlsContext::create` before `coro::run` or from `coro::offload`. A stream
  receives a pre-built context and must never load a CA bundle or register a lazy
  verify path during a handshake.
- **New blocking primitives do not enter a production loop path.**
  `src/support/` is a generic helper layer; random_device initialization and
  OpenSSL RAND calls are synchronous exceptions, not guaranteed non-blocking.
  Cryptographic failure must not fall back to the non-cryptographic PRNG. A new
  cache or retry helper that takes a lock or sleeps does not belong on a
  coroutine path. Route other blocking or CPU-bound work through `coro::offload`
  — see the inventory in [Architecture](architecture.md#blocking-operation-inventory).
- **Clock domains do not cross as absolute timestamps.** Worker future waits
  use real time; bridge scopes translate only the remaining duration into loop
  time. Session keep-alive uses loop time. Use fake transports with ManualClock
  and SystemClock for real-worker/socket tests; an auto-jumping clock cannot
  make external progress deterministic.
- **Transport simplification preserves cancellation.** Empty buffers and
  invalid streams must still observe an already cancelled scope. Keep yielding
  cancellation points in the underlying I/O, not redundantly in every wrapper.
  The `bench_tcp_prefix` target measures buffered two-byte prefixes through the
  actual TCP stream; use a separate Release build for timing, not sanitizer
  results:

  ```bash
  cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DYADDNSC_BUILD_TESTS=ON
  cmake --build build-perf --target bench_tcp_prefix --parallel
  ./build-perf/test/perf/bench_tcp_prefix --benchmark_repetitions=5
  ```
