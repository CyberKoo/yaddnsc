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

The current `YADDNSC_DRIVER_API_REVISION` is **1**. One revision corresponds
to exactly one fixed struct layout: every layout change requires bumping the
revision, and fields must never be appended while a revision stays in force —
there is no same-revision "just add fields" compatibility. A plugin reporting
any other revision is rejected at load time with a message telling you to
rebuild. Rebuild the driver against the current SDK — do not bypass the check.

This C ABI is new in this release. Drivers written against the old C++ plugin
API are completely incompatible and cannot load at all — they must be rebuilt
against the new SDK.

The host verifies a driver before use:

1. the driver exports the required entry points;
2. the descriptor's `struct_size` covers the complete revision layout;
3. the descriptor's `magic` identifies a yaddnsc driver;
4. the descriptor's `api_revision` matches the host's exactly;
5. the descriptor's string views and capability bits are valid.

### Struct layout and `struct_size`

Every ABI struct carries its size in bytes as its first field,
`struct_size`, set by the **caller** to the capacity of the struct it
actually provides:

- a value below the complete layout of the current revision — the
  `YADDNSC_*_MIN_SIZE` constants in `driver_abi.h` — is rejected with
  `YADDNSC_STATUS_INVALID_ARGUMENT`;
- a larger value may carry an unknown tail, which readers ignore. Writers
  only write fields fully inside the supplied capacity, write `struct_size`
  back as `min(capacity, sizeof(struct))`, and never read or zero the
  unknown tail.

### Views, arrays, and validation

`yaddnsc_string` / `yaddnsc_bytes` are borrowed views: `data` is not
NUL-terminated and `size` is the only valid length. A view with `size == 0`
may use `data == NULL`; a view with `size > 0` and `data == NULL` is invalid.
The same rule applies to arrays such as HTTP header lists: `count == 0` with
a `NULL` pointer is legal, `count > 0` with a `NULL` pointer is not.

Both directions validate completely. The host validates the plugin's
descriptor (non-empty name, well-formed views, supported capability bits) and
every request passed to `http_exchange` (non-empty URL, known method, header
array and per-header views, content type, body). The SDK validates the host
services table plus every host response and error report before exposing them
to driver code; an invalid error report coming back from the plugin is
surfaced as an "invalid … ABI error report" failure rather than trusted.

Response views borrowed from the host stay valid until
`yaddnsc_driver_update()` returns (host-owned arena), across multiple
exchanges.

### Cancellation and lifetime

`is_cancelled()` reports the cancellation state of the **current driver
update operation**: `0` means the operation is active, non-zero means its
cancellation token was triggered. It is driven by the same operation token as
the host HTTP exchange, so a cancelled exchange and a polled
`is_cancelled()` always agree. A driver that performs several exchanges may
poll it between calls to bail out early. It is a per-operation predicate, not
a host-global shutdown signal.

The `services` and `context` pointers are valid from
`yaddnsc_driver_create()` until the matching `yaddnsc_driver_destroy()`
returns; a plugin must not cache them. `destroy` must not throw: the C++
`Driver` base class destructor is `noexcept`, and the driver definition macro
enforces `std::is_nothrow_destructible` at compile time. Independently of
that, the host calls every plugin entry point through an exception firewall:
exceptions escaping `create`/`update`/`validate` are converted to
`YADDNSC_STATUS_INTERNAL_ERROR`, and an exception escaping `destroy` is
logged and swallowed — never retried, never replaced by a fallback.

Handle ownership follows the same rule as the firewall: on
`YADDNSC_STATUS_OK` the host owns the instance and guarantees the matching
`yaddnsc_driver_destroy()` call, while on any non-OK return the plugin must
leave `*out_driver` `NULL` — a failed create owns nothing. If a plugin
stores a handle and then reports failure, the host destroys and clears that
handle before propagating the error, so instance state never leaks out of
the create failure path.

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

Third-party drivers build against the installed yaddnsc package — no host
sources needed:

```cmake
find_package(yaddnsc CONFIG REQUIRED COMPONENTS plugin_sdk)  # optional: plugin_sdk_xml plugin_crypto

add_library(my_driver MODULE my_driver.cpp)
target_link_libraries(my_driver PRIVATE yaddnsc::plugin_sdk)
```

`plugin_sdk` is the default component. The package config resolves the Glaze
dependency automatically (`find_dependency(glaze CONFIG)`), and
`yaddnsc::plugin_sdk` carries the C++23 requirement and the SDK warning
flags, so the snippet above is the entire build setup for a JSON-only driver.

Drivers that parse XML responses link the separate `plugin_sdk_xml`
component instead: `yaddnsc::plugin_sdk_xml` implies `yaddnsc::plugin_sdk`
and adds libxml2 (`find_dependency(LibXml2)`). `driver.hpp` itself never
includes libxml2, so only drivers that include `yaddnsc/sdk/xml_raii.hpp`
need this component, and it exists only in packages built with libxml2
available.

Drivers that sign requests link the `yaddnsc::plugin_crypto` component
(HMAC/SHA/hex/base64); see below.

When developing inside the yaddnsc source tree (for example for a driver that
will be bundled), link the in-tree target instead and rebuild the project:

```cmake
add_library(<name> MODULE <name>.cpp)
target_link_libraries(<name> PRIVATE yaddnsc_plugin_sdk)
```

External and released drivers should always build against the installed
package, not the source tree.

## SDK distribution

The SDK is installed alongside the host so third-party drivers can build
without the host sources:

- headers: `<prefix>/include/yaddnsc/sdk/` (the C ABI + C++ helper layer) and
  `<prefix>/include/yaddnsc/util/` (shared string/format/URL utilities);
- CMake package: `${CMAKE_INSTALL_LIBDIR}/cmake/yaddnsc` under the prefix
  (normally `<prefix>/lib/cmake/yaddnsc/`), exporting three components —
  `yaddnsc::plugin_sdk` (the default), `yaddnsc::plugin_sdk_xml`, and
  `yaddnsc::plugin_crypto`;
- crypto helpers: a prebuilt static library with position-independent code,
  installed under `${CMAKE_INSTALL_LIBDIR}` (normally `<prefix>/lib/`), with
  its header at `<prefix>/include/yaddnsc/plugin_crypto/signing.h`. The
  target links `OpenSSL::Crypto` PUBLIC, so the OpenSSL dependency reaches
  the final driver module automatically.

The set of bundled drivers is an explicit, auditable list in
`driver/CMakeLists.txt`; adding a new bundled driver means adding its
directory to `YADDNSC_DRIVERS` there. Runtime auto-discovery (skipping
foreign libraries in the driver directory) is unaffected.

Standalone consumer examples that build against the installed package live in
`test/sdk_consumer/` (`c_abi_driver.cpp`, `http_driver.cpp`, `xml_driver.cpp`,
`crypto_driver.cpp`).

## Driver responsibilities

A driver:

- subclasses `yaddnsc::sdk::Driver` and implements `update(UpdateContext &)`;
- parses its configuration with `parse_config<T>()` from `driver_param` JSON —
  Glaze is available privately to the plugin (it is part of
  `yaddnsc::plugin_sdk`);
- performs provider HTTP calls through the Host Services exchange
  (`UpdateContext::exchange`) — the host owns the actual HTTP client.
  Hostname endpoints are resolved through the host's bootstrap DNS
  (`bootstrap_dns` or `/etc/resolv.conf`); `getaddrinfo`, `/etc/hosts` and
  NSS are never consulted, so prefer IP literals in exotic environments;
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

Drivers that need request signing can link the optional
`yaddnsc::plugin_crypto` component — a prebuilt static library
(HMAC/SHA/hex/base64).

## Standalone shared library

Standalone builds are discouraged. If unavoidable, the driver must be built as
a `MODULE` library with position-independent code against the SDK of the
exact yaddnsc version it will run with, exporting only the
`yaddnsc_driver_*` ABI entry points — the four required ones plus the
optional `yaddnsc_driver_validate` when provided — and nothing else. A
successful compilation alone does not guarantee compatibility — the
`api_revision` check decides at load time.

## Troubleshooting

- `Driver not found`: check `driver_dir`, the file name, and installation.
- Magic or `api_revision` mismatch: rebuild the driver with the current SDK
  headers.
- Missing required parameters: compare `driver_param` with the provider entry
  in [`DRIVERS.md`](../DRIVERS.md).

The SDK headers in `include/yaddnsc/sdk/` are the source of truth for the
plugin interface; this guide intentionally avoids duplicating their full API
documentation.
