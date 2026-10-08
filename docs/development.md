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

Supported current toolchains are CMake 3.28+, a 64-bit target, C++23-capable
GCC 14+, Clang 19+, or Apple Clang 15+, and OpenSSL 3.0+.

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
7. `PluginSdk.cmake` — SDK/XML handling and C ABI checks, which are built
   regardless of `YADDNSC_BUILD_TESTS`.

Keep the configuration summary and Doxygen setup between `Executable.cmake`
and `PluginSdk.cmake`. After the SDK, retain this order: `plugin_support/crypto/`,
`driver/`, `HeaderCheck.cmake`, `test/`, then `InstallRules.cmake`, preserving
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

Tests use GoogleTest/GoogleMock. Component tests may use loopback sockets,
local helper processes, or platform facilities; they do not require provider
credentials or external DNS provider access.

### Test tiers

| Tier | Location | Runs the real binary? |
|------|----------|------------------------|
| Unit | `test/unit/` | No — pure logic, no I/O |
| Component | `test/component/` | Real loopback sockets, multicast, TLS |
| Plugin ABI | `test/plugin/`, `test/sdk_consumer/` | Loads a real `.so` via dlopen |
| Integration | `test/integration/` | Yes — the built `yaddnsc` binary |
| Benchmarks | `test/perf/` | No — Google Benchmark |

There is no system-test tier: `integration_scenarios` is the highest one.

### Integration scenarios

`test/integration/run_ctest.sh` drives the real binary against a Python DNS
simulator (`test/integration/sim/server.py`, aiohttp + dnslib). It covers
three end-to-end update paths (interface/classic, http/DoT, interface/DoH),
the read-only CLI subcommands and their exit codes, and two failure
contracts that pull in opposite directions:

- an HTTP IP source returning a non-address response → the update workflow
  reports no usable IP address and nothing is published;
- no DNS answer → the update is still published, because
  `src/application/update_once.cpp` treats an unverifiable current record as
  an empty one. Pushing an unchanged record is harmless; skipping a changed
  one is not.

Run just this tier:

```bash
ctest --test-dir build-tests -R integration_scenarios --output-on-failure
```

**Check for skipped execution.** `SKIP_RETURN_CODE 77` makes CTest report
this test as skipped when `python3 -m venv` cannot bootstrap pip
(`python3-venv` missing). A successful CTest exit does not prove the scenarios
executed — use verbose output (`ctest -V -R integration_scenarios`) and check
for `ALL SCENARIOS PASSED`. Negative update scenarios require the expected
workflow diagnostic and a successful shutdown; simulator reset/log failures
are test failures, not evidence that no update was sent. The mDNS group users
(`test_mdns_coro`, `integration_scenarios`) hold the `mdns-multicast`
`RESOURCE_LOCK` and must not be run concurrently outside CTest.

### Local CI simulation

`test/ci-sim/` builds and runs the project inside Alpine containers and is a
**local** tool for reproducing the CI environment; it is not invoked by any
workflow.

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
  -o coverage.info --lcov
```

The repository defines no coverage percentage failure threshold: 80% is not an
implemented gate. Codecov upload errors can fail the job; that is not a coverage
threshold check.

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
  Host Services ABI contract tests explicitly.

`nightly.yml`:

- Linux arm64 Debug tests, Release builds with bundled/system spdlog (tests
  disabled), the dedicated `Sanitizer` build/tests, and coverage;
- `linux-amd64-clang` is **disabled** (`if: false`). There is no active Linux
  upstream-Clang CI job; macOS uses AppleClang.

Scheduled nightly builds run only when the recent-commit check passes; manual
runs bypass that check. Linux arm64 is nightly/manual coverage, not a PR check.

`yaddnsc_warnings` supplies `-Wall -Wextra -Wpedantic -Wshadow -Werror` to
production modules, the executable, header checks, and tests that link the shared
warning interfaces. SDK/C ABI/plugin-crypto targets have separate warning options;
this is not a blanket claim for all targets or dependencies. `BuildOptions.cmake`
also suppresses `maybe-uninitialized` in `Sanitizer` builds and demotes
`array-bounds` for system fmt below version 10. The conversion job adds
`-Wconversion -Wsign-conversion`; these are not default local flags.

## Formatting and static analysis

`.clang-format` is the formatting source of truth; neither workflow runs a
clang-format check, so its presence is not a CI formatting gate.

`cmake/ClangTidy.cmake` attaches clang-tidy when the unversioned `clang-tidy`
executable is found and the compiler ID matches Clang (including AppleClang).
Otherwise analysis is skipped. `.clang-tidy` enables `bugprone-use-after-move`
and `clang-analyzer-cplusplus.InnerPointer`, but not
`readability-identifier-naming` or `clang-analyzer-cplusplus.Move`. It does not
automatically enforce the naming rules. No `WarningsAsErrors` or
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
  and `plugin_support/crypto/*.h` recursively. `xml_raii.hpp` is excluded when LibXml2
  is unavailable. This is a default-build check, not coverage of every header
  in every configuration.
- **Editor feedback**: `.clangd` sets `UnusedIncludes` and `MissingIncludes`
  to `Strict`; editor diagnostics are not CI gates.

## Manual boundary review

The `architecture_guard` ctest (`cmake/ArchitectureGuard.cmake`) checks textual
boundaries — includes, layering, threads/futures, the cancellation token and the
lazy TLS trust APIs — but it cannot judge every loop-safety decision. When
touching startup, transport or IP-source code, confirm by hand:

- **`getifaddrs()` stays the only loop-thread blocking call in the IP source.**
  `ip_source/iface_util.cpp` reads a live snapshot per call; that is the
  sanctioned exception (bounded kernel metadata). Do not add a cache, a mutex or
  a single-flight wait around it.
- **A CA bundle is only ever loaded off the loop.** Build trust material with
  `net::TlsContext::create` before `coro::run` or from `coro::offload`. A stream
  receives a pre-built context and must never load a CA bundle or register a lazy
  verify path during a handshake.
- **Blocking primitives do not enter a production loop path.** `src/support/` is
  a generic helper layer; a cache or retry helper that takes a lock or sleeps does
  not belong on a coroutine path. Route blocking or CPU-bound work through
  `coro::offload` — see the blocking-operation inventory in
  [Architecture](architecture.md#blocking-operation-inventory).
