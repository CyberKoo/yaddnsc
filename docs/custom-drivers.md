# Custom Drivers

This document is for developers who want to add a DNS provider driver to
yaddnsc. End users configuring one of the bundled providers should start with
[`README.md`](../README.md) and [`DRIVERS.md`](../DRIVERS.md).

## Compatibility

Drivers are runtime-loaded shared libraries that talk to the host exclusively
through the **v1 alpha plugin ABI** — a small pure-C surface declared in
`include/yaddnsc/sdk/driver_abi.h`, plus an optional C++ helper layer in
`include/yaddnsc/sdk/driver.hpp`. No C++ exceptions, STL containers, or host
objects ever cross the `.so` boundary, so a driver does not need to share the
host's exact standard-library internals. It must still be built with a C++23
compiler on a 64-bit platform.

The host verifies a driver before use:

1. the driver magic value identifies a yaddnsc driver;
2. the descriptor's `api_revision` matches the host's exactly;
3. every ABI struct carries a `struct_size` prefix the host can safely read.

When the plugin interface changes, `api_revision` is bumped and older drivers
are rejected at load time with a message telling you to rebuild. Rebuild the
driver against the current SDK headers — do not bypass the check.

## Entry points

Four entry points are **required**: `yaddnsc_driver_get_descriptor`,
`yaddnsc_driver_create`, `yaddnsc_driver_destroy`, and
`yaddnsc_driver_update`. A fifth, `yaddnsc_driver_validate`, is **optional**
within api_revision 1: the host probes it with `dlsym` and simply skips the
driver-side `driver_param` check when it is absent, so plugins built against
an older SDK keep working.

`yaddnsc_driver_validate` lets `yaddnsc config test` check a driver's
`driver_param` against the driver's own schema without performing an update.
It is called on a live instance between create and destroy, must not use host
services (no HTTP exchange is available), and returns
`YADDNSC_STATUS_INVALID_CONFIG` with a human-readable message when the
parameter is unacceptable. The SDK's C++ helper layer emits it automatically
from `YADDNSC_DEFINE_DRIVER`; override `Driver::validate()` to add the
schema check (the bundled drivers are one-liners calling `parse_config<T>`).

## Recommended build

Place the driver under `driver/<name>/` and rebuild the project:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The driver target links only `yaddnsc_plugin_sdk`:

```cmake
add_library(<name> MODULE <name>.cpp)
target_link_libraries(<name> PRIVATE yaddnsc_plugin_sdk)
```

## SDK distribution

The SDK is installed alongside the host so third-party drivers can build
without the host sources:

- headers: `<prefix>/include/yaddnsc/sdk/` (the C ABI + C++ helper layer) and
  `<prefix>/include/yaddnsc/util/` (shared string/format/URL utilities);
- crypto helpers: `<prefix>/share/yaddnsc/plugin-sdk/plugin_crypto/`
  (`signing.h` / `signing.cpp`) — shipped as source, so the plugin compiles
  them with its own toolchain flags (PIC/sanitizer choices always match the
  plugin itself, never the host build).

The set of bundled drivers is an explicit, auditable list in
`driver/CMakeLists.txt`; adding a new bundled driver means adding its
directory to `YADDNSC_DRIVERS` there. Runtime auto-discovery (skipping
foreign libraries in the driver directory) is unaffected.

## Driver responsibilities

A driver:

- subclasses `yaddnsc::sdk::Driver` and implements `update(UpdateContext &)`;
- parses its configuration with `parse_config<T>()` from `driver_param` JSON —
  Glaze is available privately to the plugin (it is part of
  `yaddnsc_plugin_sdk`);
- performs provider HTTP calls through the Host Services exchange
  (`UpdateContext::exchange`) — the host owns the actual HTTP client;
- logs through the `YADDNSC_SDK_LOG_*` macros — the call site
  (`file` / `line` / `function`) is forwarded through Host Services to the
  host logger (`spdlog::source_loc`); there is no `-rdynamic` / `CORE_LOG`
  symbol backfill;
- reports outcomes as `yaddnsc::sdk::Error` values with the appropriate
  `YADDNSC_STATUS_*` code;
- exports itself with
  `YADDNSC_DEFINE_DRIVER(YourDriver, "<name>", "<description>", "<author>", "<version>", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)`.

Each update call runs on a fresh driver instance (`create → update →
destroy`), so instance state never leaks between updates. **Different
instances of the same module may update concurrently** (one subdomain per
task), while a single instance is never used concurrently — keep instance
state per-call and module state thread-safe. `create()` is not process-level
one-time initialization.

A plugin must not include host `src/` headers or legacy utility headers
(`CORE_LOG`, `uri.h`, `fmt.hpp`, `string_util.hpp`, host `HttpClient`); the
SDK surface (`yaddnsc/sdk/*`, backed by the shared utilities in
`yaddnsc/util/*`) plus privately linked third-party libraries is the whole
contract. String helpers (`yaddnsc/sdk/string_util.hpp`), named-argument
formatting (`yaddnsc/sdk/format.hpp`), and percent-encoding
(`yaddnsc/sdk/url_encode.hpp`) are part of that surface — use them instead of
copying implementations into the driver. Use the bundled
`driver/cloudflare/` as the reference implementation and keep
provider-specific credentials in `driver_param`.
Do not put credentials in source code or log messages — the SDK log helpers
redact sensitive request fields by default.

Drivers that need request signing can link the optional `yaddnsc_plugin_crypto`
static library (HMAC/SHA/hex/base64).

## Standalone shared library

Standalone builds are discouraged. If unavoidable, the driver must be built as
a `MODULE` library with position-independent code against the SDK headers of
the exact yaddnsc version it will run with, exporting only the four
`yaddnsc_driver_*` entry points. A successful compilation alone does not
guarantee compatibility — the `api_revision` check decides at load time.

## Troubleshooting

- `Driver not found`: check `driver_dir`, the file name, and installation.
- Magic or `api_revision` mismatch: rebuild the driver with the current SDK
  headers.
- Missing required parameters: compare `driver_param` with the provider entry
  in [`DRIVERS.md`](../DRIVERS.md).

The SDK headers in `include/yaddnsc/sdk/` are the source of truth for the
plugin interface; this guide intentionally avoids duplicating their full API
documentation.
