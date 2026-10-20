# CI and static analysis

The CI job matrix, local CI reproduction, and the formatting, static-analysis
and include-hygiene setup those jobs enforce. Running the test suites is
covered in [testing.md](testing.md); build configuration in
[development.md](development.md).

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

## Local CI simulation

`test/ci-sim/` builds and runs the project inside Ubuntu 24.04 containers —
Alpine/musl only under `--musl` — and is a **local** tool for reproducing the
CI environment; it is not invoked by any workflow.

```bash
./test/ci-sim/run.sh          # linux-amd64
./test/ci-sim/run.sh --musl   # linux-amd64-musl
./test/ci-sim/run.sh --all    # both
```


## Formatting and static analysis

`.clang-format` is the formatting source of truth. Formatting is applied from
it during development; CI enforces compilation and tests rather than style.

`cmake/ClangTidy.cmake` attaches clang-tidy when a `clang-tidy` executable is
found — the unversioned name is preferred, versioned `clang-tidy-N` names are
accepted as fallback — and the compiler ID matches Clang (including AppleClang).
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
  CI coverage comes from the nightly lint job (see above);
  the macOS job uses AppleClang, which IWYU deliberately skips.
- **Header self-containment**: in non-Release builds, `yaddnsc_header_checks`
  compiles individual headers matching `src/*.{h,hpp}`, `include/*.{h,hpp}`,
  `driver/*/*.{h,hpp}` recursively (including signing under the SDK tree).
  `xml_raii.hpp` is excluded when LibXml2 is unavailable. This is a
  default-build check, not coverage of every header in every configuration.
- **Deprecated C headers**: the `architecture_guard` ctest rejects the C
  spelling (`<stdint.h>` and friends) in every C++ translation unit under
  `src/`, `include/`, `driver/`, and `test/` — clang-tidy's
  `modernize-deprecated-headers` only inspects the main file, so this check
  covers the rest. The plugin C ABI header (compiled as C11) and the frozen
  ABI baseline copy are exempt.
- **Editor feedback**: `.clangd` sets `UnusedIncludes` and `MissingIncludes`
  to `Strict`; editor diagnostics are not CI gates.
