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

The current ABI version is **1.0** (`YADDNSC_DRIVER_ABI_MAJOR` 1,
`YADDNSC_DRIVER_ABI_MINOR` 0). The host is the provider. It loads a plugin
whose major matches and whose minor is no higher than the host's, and a
plugin accepts a services table on the same rule. A plugin's minor is the
minimum host minor it requires.

Within a major, fields are only appended to the versioned structs. New host
callbacks, capability bits, status codes, log levels, and HTTP methods are
minor bumps. Moving or removing a field, growing a leaf type, or adding a
required entry point is a major bump. `YADDNSC_*_MIN_SIZE` is the ABI 1.0
baseline and stays there when a later minor appends a field. A different
major, or a plugin that requires a newer minor, is rejected at load time.
Rebuild the driver against the current SDK — do not bypass the check.

This C ABI is new in this release. Drivers written against the old C++ plugin
API are completely incompatible and cannot load at all — they must be rebuilt
against the new SDK.

Supported platforms for this ABI are Linux ELF x86_64, Linux ELF AArch64, and
macOS 64-bit. The host loads plugins with `dlopen` (`RTLD_NOW | RTLD_LOCAL`).
The layout assumes `sizeof(void*) == 8`, `sizeof(size_t) == 8`, and default
struct packing. Windows is not a supported host.

A plugin is in-process trusted code, not a sandbox. It can abort, loop
forever, corrupt the host's memory, or call the operating system directly.
The ABI checks do not contain an untrusted plugin.

The host verifies a driver before use:

1. the driver exports the required entry points;
2. the descriptor's `struct_size` covers the 8-byte version prefix
   (`struct_size`, `abi_major`, `abi_minor`);
3. `abi_major` is a major the host implements, and the host's minor is at
   least the plugin's minor;
4. `struct_size` covers that minor's baseline (the ABI 1.0 baseline for
   minor 0);
5. the descriptor's `magic` identifies a yaddnsc driver;
6. the descriptor's string views and capability bits are valid. The version
   check runs before the capability check.

### Struct layout and `struct_size`

The versioned top-level structs carry their size in bytes as the first field,
`struct_size`, set by the **caller** to the capacity of the struct it
actually provides. Those structs are `yaddnsc_error`, `yaddnsc_http_request`,
`yaddnsc_http_response`, `yaddnsc_update_request`,
`yaddnsc_driver_descriptor`, and `yaddnsc_host_services`.

Leaf types do not have `struct_size` and are not prefix-compatible:
`yaddnsc_string`, `yaddnsc_bytes`, `yaddnsc_source_location`, and
`yaddnsc_http_header`. Do not append fields to them.

For a versioned struct:

- a value below the ABI 1.0 baseline — the `YADDNSC_*_MIN_SIZE` constants in
  `driver_abi.h` — is rejected once the version has been accepted;
- a larger value may carry fields added by a later minor. Readers use
  `yaddnsc_struct_has_field` before touching those fields and ignore a tail
  they do not know. Writers only write fields fully inside the supplied
  capacity, write `struct_size` back as `min(capacity, sizeof(struct))`, and
  never read or zero the unknown tail.

### Views, arrays, and validation

`yaddnsc_string` / `yaddnsc_bytes` are borrowed views: `data` is not
NUL-terminated and `size` is the only valid length. A view with `size == 0`
may use `data == NULL`; a view with `size > 0` and `data == NULL` is invalid.
The same rule applies to arrays such as HTTP header lists: `count == 0` with
a `NULL` pointer is legal, `count > 0` with a `NULL` pointer is not.

Every message view the ABI returns is borrowed. Copy it before the call
returns. Do not cache it.

An HTTP request body uses `data == NULL` as a presence bit:

| `data` | `size` | Meaning |
| --- | --- | --- |
| `NULL` | 0 | no body |
| non-`NULL` | 0 | a body of length 0 |
| non-`NULL` | N | N bytes |
| `NULL` | N | invalid |

The C++ helper matches this with `std::nullopt` (no body) and an engaged
`std::string`, including an empty one (a body is present).

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
cancellation token was triggered. The return type is `int32_t`. It is a
per-operation predicate, not a host-global shutdown signal. A result of `0`
does not promise that the next HTTP exchange will avoid `CANCELLED`:
cancellation can happen between the check and the exchange. If the operation
is already cancelled when an exchange starts, the host does not send the
request. A driver that receives `CANCELLED` stops the current update. A
driver that performs several exchanges may poll between calls to bail out
early.

The `services` and `context` pointers may be saved during
`yaddnsc_driver_create()` and stay valid until the matching
`yaddnsc_driver_destroy()` returns. Do not use them after that, and do not
hand them to another thread. `destroy` must not throw: the C++
`Driver` base class destructor is `noexcept`, and the driver definition macro
enforces `std::is_nothrow_destructible` at compile time. Independently of
that, the host calls every plugin entry point through an exception firewall:
exceptions escaping `create`/`update`/`validate` are converted to
`YADDNSC_STATUS_INTERNAL_ERROR`, and an exception escaping `destroy` is
logged and swallowed — never retried, never replaced by a fallback.

Handle ownership: on `YADDNSC_STATUS_OK`, `*out_driver` is non-NULL and the
host owns the instance, including the matching `yaddnsc_driver_destroy()`
call. `OK` with a NULL handle is a contract violation; the host turns it
into `INTERNAL_ERROR` and owns nothing. On any non-OK return the plugin must
leave `*out_driver` `NULL` — a failed create owns nothing. The host clears
`*out_driver` before calling the plugin. If a plugin stores a handle and
then reports failure, the host destroys and clears that handle before
propagating the error.

## Entry points

Four entry points are **required**: `yaddnsc_driver_get_descriptor`,
`yaddnsc_driver_create`, `yaddnsc_driver_destroy`, and
`yaddnsc_driver_update`. A fifth, `yaddnsc_driver_validate`, is **optional**
since ABI 1.0: the host probes it with `dlsym`. When the symbol is absent,
the plugin still loads and can perform updates, and `yaddnsc config test`
fails because it cannot confirm `driver_param`. `YADDNSC_DEFINE_DRIVER`
always exports the entry. The C++ helper's default `Driver::validate()`
returns success, so config test passes and no schema check runs. Override
it.

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

The services table may be stored on the instance from `create` until
`destroy`. Do not hand it to another thread, and do not use it after
`destroy` returns. Do not cache response views past the return of
`yaddnsc_driver_update()`. Host callbacks do not re-enter plugin entry
points. `http_exchange` may be called only from `update`, on that call's
thread. `create`, `destroy`, and `validate` must not perform an exchange.
During `validate` the host's exchange entry fails with `INVALID_ARGUMENT`
and does not touch the network.

`capabilities` is enforced by the host before the plugin is called. An
update whose record type is `A` requires `YADDNSC_DRIVER_CAPABILITY_A`;
`AAAA` requires `YADDNSC_DRIVER_CAPABILITY_AAAA`. Any other record type,
including `TXT`, stops at that gate. The gate produces no plugin ABI
status. This host reports it as `domain::DriverError::Code::UPDATE_FAILED`.
`YADDNSC_STATUS_UNSUPPORTED_RECORD` is the status a plugin returns when it
refuses a record; the gateway maps that return to the same domain code.
Zero capabilities is legal at load and means the plugin accepts no ABI 1.0
record type. Unknown bits are rejected at load. ABI 1.0 has no capability
bit for any other record type. A new bit is a minor bump.

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
`abi_major` / `abi_minor` check decides at load time.

## Error reports and HTTP

`out_error` may be NULL. A NULL pointer, or a `struct_size` below the
ABI 1.0 baseline, is left unchanged; the returned status is still the
result. `YADDNSC_STATUS_OK` does not write `out_error`. Any other status,
when the struct is large enough, sets `out_error->status` to that same
status. `retry_after_seconds == 0` means there is no hint. Any non-OK status
may set it; the host uses it only when it is greater than 0.

On this host, an unknown status makes the error report illegal, an unknown
capability bit rejects the plugin at load, an unknown HTTP method returns
`INVALID_ARGUMENT`, and an unknown log level is recorded as INFO.

`http_exchange` returns non-OK only for transport failure (`NETWORK_ERROR`)
or cancellation (`CANCELLED`). A provider status, including 4xx and 5xx and
integers outside 100–599, arrives as `STATUS_OK` plus
`out_response->status_code`. The SDK does not range-check that integer. The
body is binary-safe. Response headers may repeat and keep the casing the
peer sent. Compare header names case-insensitively. Trailers are not exposed.

Request headers named `Host`, `Content-Length`, `Content-Type`, `Connection`,
and `User-Agent` are discarded; the host compares those names
case-insensitively and supplies them itself. `Host` comes from the URL.
`User-Agent` is the host's single value. `Content-Length` comes from the
body. `Connection` is sent only when the host's policy requires it.
`Content-Type` is taken only from the `content_type` field, and only when a
body is present (`data != NULL`). `Transfer-Encoding`, `Trailer`, and
`Upgrade` are not sent; the exchange fails.

`Driver::run_update` classifies a response that `check_response` rejects.
401 and 403 become `AUTHENTICATION_FAILED`. 429 becomes `RATE_LIMITED`, and
the first `Retry-After` value — a delay in seconds, or an IMF-fix HTTP-date
measured from now — is copied into `retry_after_seconds`. An unparsable or
already-past date contributes no hint. Every other rejection stays
`UPSTREAM_REJECTED` with `retry_after_seconds` 0. A `check_response` that
accepts the response stays a success.

This host allows only `http` and `https`. It follows redirects, up to 10.
301, 302, and 303 become GET and drop the body; 307 and 308 keep the method
and the body. A redirect from https to http is not followed. The plugin sees
the response after redirects. One exchange is limited to 64 KiB of headers
and 16 MiB of body.

## Troubleshooting

- `Driver not found`: check `driver_dir`, the file name, and installation.
- Magic mismatch, a different ABI major, or a plugin minor newer than the
  host: rebuild the driver with the current SDK headers.
- Missing required parameters: compare `driver_param` with the provider entry
  in [`DRIVERS.md`](../DRIVERS.md).

The SDK headers in `include/yaddnsc/sdk/` are the source of truth for the
plugin interface; this guide intentionally avoids duplicating their full API
documentation.
