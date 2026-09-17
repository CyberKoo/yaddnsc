# Development

This document covers building, testing, coverage, sanitizers, and generated
API documentation. It is not required for normal yaddnsc operation.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

Use `-DCMAKE_BUILD_TYPE=Release` for production builds.

Supported current toolchains are CMake 3.28+, C++23-capable GCC 14+,
Clang 19+, or Apple Clang 15+, and OpenSSL 3.0+.

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

## Coverage

The CI coverage job builds with GCC coverage instrumentation, runs the complete
CTest suite, and reports line and branch coverage. For a local report, use the
same toolchain and run `gcovr` or `fastcov` after clearing stale profile files.

Do not treat third-party or test code as project coverage. Branch coverage is
more useful than line coverage for protocol parsers, error handling, and
configuration validation.

## Sanitizers

Debug builds enable project sanitizers by default. Disable them for coverage
instrumentation when the tools are incompatible:

```bash
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DYADDNSC_SANITIZE_DEBUG=ON \
  -DYADDNSC_BUILD_TESTS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

Use `YADDNSC_SANITIZE_DEBUG=OFF` for coverage builds when necessary. Keep
sanitizer failures actionable; do not hide them with broad suppressions.

## Documentation

Doxygen is optional:

```bash
cmake -S . -B build-docs \
  -DCMAKE_BUILD_TYPE=Release \
  -DYADDNSC_BUILD_DOCS=ON
cmake --build build-docs --target doxygen
```

## Dependency maintenance

Dependencies are declared through CPM.cmake and pinned in the project build
configuration. Dependency upgrades require a normal review of compatibility,
licenses, and security advisories.

## CI and warning gates

The repository runs compiler/platform matrices and dedicated warning gates.
End-user build instructions live in the README; this section documents what
the workflows actually cover.

`ci.yml` (push/PR):

- `linux-amd64` — GCC Debug + full test suite (includes the plugin contract
  tests and the `architecture_guard` boundary checks);
- `linux-amd64-musl`, `macos-arm64` — platform matrix (glibc/musl/macOS);
- `conversion-gate` (PR only) — `-Wconversion -Wsign-conversion` build;
- `benchmark` (PR only) — Google Benchmark smoke run;
- `plugin-contract` — builds the whiteboard test plugin and runs the dlopen /
  Host Services ABI contract tests explicitly.

`nightly.yml`:

- `linux-arm64`, Release + `system-spdlog` feature flags, `Sanitizer` build
  type (ASan/UBSan, never combined with Release), and coverage (trend
  observation via Codecov, not a hard gate);
- `linux-amd64-clang` is present but **disabled** (`if: false`) because it is
  too slow — the platform matrix does NOT currently include Clang.

Strict warnings (`-Wall -Wextra -Wpedantic -Wshadow -Werror`) apply to
production targets and test targets alike; test warnings are fixed, not
silenced.
