# Development

This document records build commands and project maintenance processes;
test and coverage commands live in [testing.md](testing.md), CI jobs and
static analysis in [ci.md](ci.md). Contributor requirements live in
[rules/](../rules/), including
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
[Include hygiene](ci.md#include-hygiene)).

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
rules in its header comment are the contract — stated for readers in
[plugin-abi.md](plugin-abi.md). The C++ helper layer
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
4. honor the `YADDNSC_*_MIN_SIZE` freeze rule in
   [plugin-abi.md](plugin-abi.md#compatibility): never retarget an existing
   macro. A new container gets its own `YADDNSC_<TYPE>_MIN_SIZE`
   (e.g. `YADDNSC_HOST_SERVICES_V1_1_MIN_SIZE`);
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

