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

### Deterministic timing regressions

Concurrency tests establish ordering through entry/retirement notifications and
release gates, not fixed sleeps or total wall-time assertions. A started flag
alone does not keep a worker alive: the worker stays behind its gate until the
cancellation or queue state under test has been observed. TLS capture ends on a
complete ClientHello, not a quiet interval, and logging overflow keeps its drain
gate closed until after the counter is inspected.

Pure timer semantics run on `ManualClock`; the busy-ready-queue test advances
that clock at a known checkpoint while ready work remains. Async gateway tests
that also rendezvous with real offloads use a paused test clock, so an unrelated
exchange deadline cannot end the scenario before cancellation. The production
synchronous bridge's worker deadline remains real where that deadline is the
subject of the test; it is not compared with a server response delay or a
latency threshold. Sync cancellation regressions additionally require the
plugin-observed `CANCELLED` status: a worker-deadline `NETWORK_ERROR` fallback
cannot satisfy them. On Linux, a test-only poll wrapper checks pending SIGTERM
delivery at an atomic-unmask kernel wait boundary; registration-only coverage
is kept separately on all supported POSIX targets.
Retry-After date arithmetic is checked against a supplied,
fixed clock sample, while the normal SDK boundary still samples system time.

Gate-based test binaries have a 120-second CTest watchdog. It diagnoses a
stalled test process; it does not release a fixture, choose a functional result,
or prove that an operation was fast enough. Manual invocations of those binaries
should likewise use an external runner timeout.

### Coroutine cancellation regressions

`test_coro_lifecycle` covers execution retirement, nursery resource ownership,
intrusive-list and forward-list contracts and cancellation arbitration. Its death tests also run
in Release, where production failure handling must not depend on assertions.

Frame-destructor reentrancy is covered there too, because a parameter destructor
runs inside the loop's retirement transaction: unregistering its own timer and fd
registration, cancelling a sibling's parked wait, posting to the inbox and
spawning into an open nursery each have a case. Those cases assert observable
state — a dispatch count that stays zero, a resumed waiter, a batch-order
observation — never elapsing real time: timers run on `ManualClock`, whose
advancement is controlled by the test and idle-loop deadline jumps, not OS
scheduling. A lost disarming shows up as a checked deadline jump instead of a
sleep bet.
`test_coro` covers the paced I/O scan in whole resumed frames:
`DRAIN_POLL_FRAME_BUDGET` frames into a self-refilling ready chain the loop still
polls without blocking, and a new fd registration forces a scan ahead of that
budget. Both record the frame index as a GoogleTest XML property, so tuning the
budget is a visible change rather than a silently retuned test.
`test_coro_allocation_failure` additionally checks control-allocation counts,
no-allocation shutdown/drain and cleanup after error-aggregation failure. Frame
and retained-handle storage measurements are available as GoogleTest XML
properties in its allocation-counting cases.

```bash
cmake --build build-tests --target test_coro_lifecycle test_coro_allocation_failure --parallel 2
ctest --test-dir build-tests -R '^test_coro_(lifecycle|allocation_failure)$' --output-on-failure
```

The runtime suite covers reusable mutex/sleep operations, cancellable Handle
reservations and offload admission/capture release. `test_net_coro_unit` covers
fd-operation reuse, while runtime tests exercise concurrent lock descriptors,
Handle reservation conflicts, FIFO admission and capacity held through running
cancellation. `test_coro_allocation_failure` checks pool initialization and both
immediate/deferred submission failures, as well as mutex queue growth failure.
It also checks fd registration rollback when table or dispatch-buffer growth
fails. On Linux, `test_coro_syscall_failure` combines sustained poll `ENOMEM`
with allocator failure to verify that shedding completes without allocating.
It also registers descriptors from a callback during both readiness dispatch
and sustained-failure shedding, verifying that later snapshot callbacks run.
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

`bench_coro_lifecycle` keeps inline, fan-out, race and destructor-overlay actors
in separate translation units from registration and loop setup. This reduces
cross-scenario layout coupling but does not eliminate compiler/linker layout
sensitivity; compare both runtime variants before and after benchmark changes.
`BM_reservation_pressure/children/mixed/cancel` adds 4–1024 concurrent result
reservations, optionally alternating int/long producers and cancelling half the
claimed consumers before they resume. Recovery checks publication order and
exactly-once consumption. It reports CPU time per batch and allocations per
batch (unlike the older cases' amortized inner-operation counters); throughput
counts producer results. The same scenario runs in `test_coro` at 4–256 children.
Use interleaved fixed-iteration runs and retain individual samples rather than
attributing a benchmark-only layout change to the runtime.

`bench_coro_observation` is a separate binary so latency/residency instrumentation
cannot perturb the throughput benchmark's code layout. `BM_ready_burst/children/mode`
releases 8–4096 children into one batch. Mode 0 is light; mode 1 adds 256 unsigned
mixing rounds to each resumption, mode 2 to each frame-parameter destructor.
A timer becomes due and an empty pipe receives a byte during the first
resumption. ManualClock controls timer eligibility only: service, execution and
retirement spans use the real monotonic clock. This measures a controlled
in-burst service delay, not production-clock deadline lateness or external
kernel-arrival latency. The retirement span covers the first through last
parameter destructor, including intervening reaper work, not the entire reaper.
Each repetition is exactly one fresh-loop observation; retain JSON `iteration`
rows for percentile analysis rather than treating aggregate rows as samples.

`BM_payload_residency/children/bytes/hold_ms/consume_early` uses the system clock
for actual 1-second residence, plus 60-second 1024 × 4096-byte cases. It records
owned payload buffer bytes and producer-frame counts at 0%, 10% and 100% of the
hold. It verifies that consumption or nursery exit releases the payload,
including when invalid Handle copies remain. These counters are not RSS,
array/allocator overhead, usable sizes or entry/control-block bytes. The same
observation workloads have formal lifecycle tests with manual-clock holds;
those tests verify ordering/ownership, not timing budgets or a real-time soak.

```bash
cmake --build build-release --target bench_coro_observation --parallel 1
build-release/test/perf/bench_coro_observation --benchmark_filter=BM_ready_burst \
  --benchmark_repetitions=512 --benchmark_out=latency.json --benchmark_out_format=json
# Excludes the two 60-second cases; run those separately when needed.
build-release/test/perf/bench_coro_observation --benchmark_filter='BM_payload_residency/.*/1000/' \
  --benchmark_repetitions=3 --benchmark_out=residency.json --benchmark_out_format=json
```

Pin a CPU when comparing distributions, retain raw repetitions and report
machine/load limitations. No timer/I/O latency SLO, payload retention limit or
automatic admission/expiry policy is implied by these observations.

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
The final runtime DEB is built separately via `docker/build-deb.sh`
(local Docker build, see above); this repository has no release workflow.

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

The `coverage` job in `ci.yml` runs on code pushes and pull requests. It uses
GCC instrumentation with sanitizers disabled, runs the CTest suite, and uploads
an LCOV report to Codecov using the repository's `CODECOV_TOKEN` secret. A failed
test or coverage gate does not prevent uploading a successfully generated report;
the job still fails. Report-generation and upload errors also fail the job.
Nightly does not repeat this job. For a local report, use GCC 14, an installed
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
  -i "$(pwd)/src/" "$(pwd)/include/" "$(pwd)/driver/" \
  -e "/usr/" "*/generated/" \
  -b -o coverage.info --lcov
```

The checkout-root include paths exclude third-party CPM headers. `-b` keeps
useful branches but excludes compiler-generated exceptional branches, matching
the CI gate's existing baseline; `-B` includes those additional branches and is
not directly comparable. GCC's coroutine split clones bodies into
`.actor`/`.destroy` functions, and gcov's line table collapses those clones away.
Coroutine bodies can therefore largely disappear from the line report. Read
the branch column alongside the line column rather than treating high line
coverage as proof that cancellation, timeout and redirect paths were exercised.

Even the branch numbers have a hard ceiling: gcov reports only the *ramp*
function's branch table, and much of it is coroutine-lowering plumbing
(state-machine dispatch, frame EH edges, `bad_alloc` unwinds) that no test can
reach — several coroutine TUs sit at their proven artifact ceiling (e.g.
`update_task.cpp` 33%, `transport.cpp` 32%) with every behavioural branch
already covered. Chase behaviour, not the number.

### Coverage gate

`test/coverage_gate.py` enforces a two-track gate on the tracefile (the
CI coverage job runs it before the Codecov upload):

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

The `ThreadSanitizer` build type is a second dedicated configuration for
data-race detection; TSan cannot be combined with ASan/UBSan, so it is a
separate build type rather than an extension of `Sanitizer`. The CI
`thread-sanitizer` job runs it on Ubuntu only — GCC's libtsan does not
support musl, and C++23 coroutines under Darwin TSan are unverified:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=ThreadSanitizer -DYADDNSC_BUILD_TESTS=ON
cmake --build build-tsan --parallel
TSAN_OPTIONS="halt_on_error=0 history_size=7 second_deadlock_stack=1 report_bugs=1" \
  ctest --test-dir build-tsan --output-on-failure --timeout 300 -E '^test_stop_signals$'
```

TSan reports make the test binaries exit non-zero, so a green run means zero
warnings. `test_stop_signals` is excluded: its death test forks a child whose
asynchronous second SIGINT can land inside TSan-intercepted code and deadlock
libtsan, and the suite checks signal dispositions and exit codes rather than
shared state, so TSan has nothing to detect there. The `--timeout` bounds any
other hung test instead of waiting out the CI job timeout.

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
backoff, the unchanged caller window and cancellation during backoff. The
successful receive case uses the real loop clock with a 5s test deadline, so
UDP delivery and timeout share the same clock; its two 1ms backoffs are checked
as a lower bound. Cancellation cases use a manual clock for exact deadline
assertions. Debug sanitizer builds exercise these same tests; a passing suite
does not substitute for separate TSan/platform validation.

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
- adding an optional host-services extension container whose first member is
  the unchanged previous table (`yaddnsc_host_services_v1_1`); the provider
  passes `&base` to the existing entry point and `base.struct_size` covers the
  whole container. This is the preferred form when growing the 1.0 type would
  break existing aggregate initializers compiled with
  `-Wmissing-field-initializers -Werror`;
- adding a host-services callback, capability bit, status code, log level,
  or HTTP method;
- adding an async execution group (async create/destroy/begin/resume, optional
  async validate) and its services/event types. The async symbols are
  conditional on async mode, never common required entry points for every
  ABI 1.x plugin.

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
   forbidden). For the host-services extension container, the old table is
   the first member at offset 0 and the new field starts at the old table's
   `sizeof`; the 1.0 type itself does not grow;
2. read every post-1.0 field behind `yaddnsc_struct_has_field`;
3. pin new constant values in the assertion block of `driver_abi.h`, and
   update the `YADDNSC_DRIVER_ABI_MINOR` pin there;
4. keep the `YADDNSC_*_MIN_SIZE` macros pointed at the ABI 1.0 baseline —
   never retarget them. A new container gets its own
   `YADDNSC_<TYPE>_MIN_SIZE` (e.g. `YADDNSC_HOST_SERVICES_V1_1_MIN_SIZE`);
   that is a new baseline, not a retarget of the 1.0 one;
5. keep `test_abi_freeze_compat` green: the frozen v1.0 plugin under
   `test/plugin/abi_baseline/` compiles against the frozen 1.0 header copy
   and must keep loading and running unchanged;
6. never edit the frozen copy — CI verifies its sha256
   (`test/plugin/abi_baseline/v1_0/driver_abi.h.sha256`);
7. keep sync and async execution groups mutually exclusive at load: a
   new-protocol module must export exactly one complete group, while a
   minimum-0 legacy sync module keeps its old sync path even when it also
   exports async names.

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
- `coverage` — GCC coverage build + full test suite, the two-track coverage
  gate, and Codecov upload (including reports from failed tests/gates);
- `thread-sanitizer` — `ThreadSanitizer` build type + full test suite minus
  `test_stop_signals` (see "Sanitizers" for the exclusion and the platform
  rationale);
- `conversion-gate` (PR only) — `-Wconversion -Wsign-conversion` build;
- `benchmark` (PR only) — Google Benchmark smoke run, with no stored historical
  baseline, comparison step, or performance-regression threshold;
- `plugin-contract` — builds the whiteboard test plugin and runs the dlopen /
  Host Services ABI contract tests explicitly; also verifies the frozen ABI 1.0
  baseline header by sha256 and runs the frozen-plugin compatibility test
  (`test_abi_freeze_compat`).

`nightly.yml`:

- Linux arm64 Debug tests, Release builds with bundled/system spdlog (tests
  disabled), and the dedicated `Sanitizer` build/tests;
- `linux-amd64-lint` — compile-only static analysis under upstream Clang
  (no tests run): IWYU violations fail the build, compiler warnings fail via
  `-Werror`, and clang-tidy diagnostics fail via `WarningsAsErrors: '*'`.
  macOS uses AppleClang.

Scheduled nightly builds run only when the recent-commit check passes; manual
runs bypass that check. Scheduled runs execute from the default branch's
workflow file but check out, build, and test the `dev` branch (`NIGHTLY_REF`);
manual runs build the branch selected in the dispatch UI. Linux arm64 is
nightly/manual coverage, not a PR check.

`yaddnsc_warnings` supplies `-Wall -Wextra -Wpedantic -Wshadow -Werror` to
production modules, the executable, header checks, and tests that link the shared
warning interfaces. SDK/C ABI/plugin-crypto targets carry their own warning
options. `BuildOptions.cmake`
also suppresses `maybe-uninitialized` in `Sanitizer` and `ThreadSanitizer`
builds and demotes
`array-bounds` for system fmt below version 10. The conversion job adds
`-Wconversion -Wsign-conversion` on top of the defaults above.

## Formatting and static analysis

`.clang-format` is the formatting source of truth. Formatting is applied from
it during development; CI enforces compilation and tests rather than style.

`cmake/ClangTidy.cmake` attaches clang-tidy when a `clang-tidy` executable is
found — the unversioned name is preferred, versioned `clang-tidy-N` names are
accepted as fallback — and the compiler ID matches Clang (including AppleClang).
The nightly `linux-amd64-lint` job exercises it under upstream Clang.
Otherwise analysis is skipped. `.clang-tidy` enables a long `Checks` list of
roughly 150 entries across bugprone, clang-analyzer, cert, cppcoreguidelines,
google, hicpp, misc, modernize, performance, readability and concurrency —
including `bugprone-use-after-move` and `clang-analyzer-cplusplus.InnerPointer`,
but not `readability-identifier-naming` or `clang-analyzer-cplusplus.Move`. It
does not automatically enforce the naming rules. `WarningsAsErrors: '*'` turns
every enabled diagnostic into a build failure, so the lint job gates on tidy
findings; intentional constructs (ld `--wrap` symbol names, coroutine promise
protocol methods, moved-from state assertions) are annotated in source with a
check-named `NOLINT` and a reason. Tool execution/parsing errors may
still fail the build.

## Include hygiene

- **IWYU** (`cmake/IWYU.cmake`) is enabled by default only when the tool is
  found and the compiler is upstream Clang, not GCC or AppleClang. It uses
  `--error` to fail on violations and `.iwyu-mappings.imp` for mappings.
  Activation occurs after third-party targets are created; generated header-check
  translation units opt out. Disable locally with `-DYADDNSC_IWYU=OFF`.
  CI coverage comes from the nightly `linux-amd64-lint` job (upstream Clang);
  the macOS job uses AppleClang, which IWYU deliberately skips.
- **Header self-containment**: in non-Release builds, `yaddnsc_header_checks`
  compiles individual headers matching `src/*.{h,hpp}`, `include/*.{h,hpp}`,
  `driver/*/*.{h,hpp}` recursively (including signing under the SDK tree).
  `xml_raii.hpp` is excluded when LibXml2 is unavailable. This is a
  default-build check, not coverage of every header in every configuration.
- **Editor feedback**: `.clangd` sets `UnusedIncludes` and `MissingIncludes`
  to `Strict`; editor diagnostics are not CI gates.
