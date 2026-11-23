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
  (`test_abi_freeze_compat`);
- `sdk-package` — builds the Ubuntu DEB development package, installs it in a
  clean container, and runs SDK consumer checks.

`nightly.yml`:

- Linux arm64 Debug tests, Release builds with bundled/system spdlog (tests
  disabled), and the dedicated `Sanitizer` build/tests;
- `native-packages` — x86_64 package generation on Debian 13, Ubuntu
  24.04/26.04, Fedora 43, openSUSE Leap 16.0 / Tumbleweed, and Alpine;
- `native-packages-arm64` — AArch64 package generation on Debian 13, Ubuntu
  24.04/26.04, Fedora 43, openSUSE Leap 16.0 / Tumbleweed, and Alpine. Both
  matrices run installation, SDK consumer, and configuration-upgrade checks
  before exporting packages;
- `nightly-release` — after the complete nightly test, lint, and package matrix
  passes, updates the same `nightly` pre-release in place with the pinned commit,
  notes, and x86_64 / AArch64 assets. Fixed asset names replace the previous
  build, and stale assets are removed after the new upload succeeds. Asset names
  start with `yaddnsc-` and include the distribution and architecture, for
  example `yaddnsc-ubuntu-24.04_arm64.deb` and
  `yaddnsc-dev-ubuntu-24.04_arm64.deb`;
- `docker-hub` — after the same complete matrix passes, builds the Alpine-based
  image for `linux/amd64` and `linux/arm64` and pushes it as
  `cyberkoo/yaddnsc:nightly`;
- `linux-amd64-lint` — compile-only static analysis under upstream Clang
  (no tests run): compiler warnings fail the build via `-Werror`; IWYU
  findings are reported without `--error` and do not fail the build;
  clang-tidy findings fail it only for the defect-prone families
  (`bugprone-*`, `clang-analyzer-*`, `cert-*`), the rest are warnings.
  macOS uses AppleClang.

Scheduled nightly builds run only when the recent-commit check passes; manual
runs bypass that check. Scheduled runs execute from the default branch's
workflow file but check out, build, and test the `dev` branch (`NIGHTLY_REF`);
manual runs build the branch selected in the dispatch UI. Every job uses the
same commit selected by `should_run`. Only `dev` commits without an exact
release/pre-release tag can update the public nightly; other manual branches
produce workflow artifacts only. Linux arm64 is nightly/manual coverage, not a
PR check.

Nightly versions derive from the source, not workflow run numbers. The
first-parent commit count is the snapshot sequence, so skipped runs consume no
number and rerunning a commit reproduces its version. DEB/RPM use
`<version>~dev.<count>.g<sha>`; APK uses
`<version>_alpha0_git<count>-r0`. APK `pkgrel` and RPM `Release` therefore reset
normally for each new source version, and a stable release sorts above every
nightly of the same minor version. Official alpha/rc tags and nightly are
separate pre-release channels and do not promise automatic cross-channel
upgrades.

`release-docker.yml` runs when a stable GitHub release is released. It builds the
same Alpine-based `linux/amd64` and `linux/arm64` image from the release tag and
pushes both `cyberkoo/yaddnsc:<release-tag>` and `cyberkoo/yaddnsc:latest`.
Pre-releases do not trigger this workflow or move `latest`. Both Docker publishing
jobs authenticate with the `DOCKER_USERNAME` and `DOCKER_PASSWORD` Actions
secrets.

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
does not automatically enforce the naming rules.
`WarningsAsErrors: 'bugprone-*,clang-analyzer-*,cert-*'` promotes only the
defect-prone families to errors: a bugprone, clang-analyzer or cert finding
fails the build, while the suggestion-style families (modernize, readability,
performance, hicpp, google, misc) are reported at warning severity. That split
keeps the checks that catch real defects fatal and takes the noisy,
toolchain-version-dependent findings out of the gate. Intentional constructs
(ld `--wrap` symbol names, coroutine promise protocol methods, moved-from state
assertions) are annotated in source with a check-named `NOLINT` and a reason.
clang-tidy is invoked with `--extra-arg=-Wno-error` so that the project's
`-Werror`, which reaches it through the shared compile command, does not turn
the compiler warnings it re-reports into a build failure — the real compile
already enforces those. Tool execution/parsing errors may still fail the build.
`cmake/BuildOptions.cmake` activates clang-tidy and IWYU only after every
bundled third-party target has been created, including the test-only
GoogleTest/GoogleMock and Google Benchmark targets in
`test/Dependencies.cmake`. Static analysis therefore applies to first-party
translation units without package-specific suppressions.

## Include hygiene

- **IWYU** (`cmake/IWYU.cmake`) is enabled by default only when the tool is
  found and the compiler is upstream Clang, not GCC or AppleClang. It runs
  without `--error`, so violations are reported but do not fail the build, and
  it uses `.iwyu-mappings.imp` for mappings.
  Activation follows the dependency-first ordering described above; generated
  header-check translation units opt out. Disable locally with
  `-DYADDNSC_IWYU=OFF`.
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
