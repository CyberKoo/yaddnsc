# Custom Drivers

This document is for developers who want to add a DNS provider driver to
yaddnsc. End users configuring one of the bundled providers should start with
[`README.md`](../README.md) and [`DRIVERS.md`](../DRIVERS.md).

## Compatibility

Drivers are runtime-loaded shared libraries that talk to the host exclusively
through the **v1 plugin ABI** — a small pure-C surface declared in
`include/yaddnsc/sdk/driver_abi.h`, plus an optional C++ helper layer in
`include/yaddnsc/sdk/driver.hpp`. No C++ exceptions, STL containers, or host
objects ever cross the `.so` boundary, so a driver does not need to share the
host's exact standard-library internals. The two halves have different build
requirements: `driver_abi.h` is plain C11 depending on nothing but
`<stddef.h>` and `<stdint.h>`, so a driver written against that header alone
needs no C++ at all — `test/sdk_consumer/c_abi_driver.cpp` is a complete one.
The optional C++ helper layer needs a C++23 compiler. Either way the platform
must be 64-bit and little-endian.

The current ABI version is **1.0** (`YADDNSC_DRIVER_ABI_MAJOR` 1,
`YADDNSC_DRIVER_ABI_MINOR` 0), and **the ABI is stable since the 1.0.0
release**: a driver built against ABI 1.0 loads and runs on every host
implementing ABI 1.x. The ABI was frozen at 1.0.0-alpha.5, so drivers built
against alpha.5 or later already implement ABI 1.0; drivers built against
alpha.1–alpha.4 predate the freeze and must be recompiled.
Three version numbers exist and only the first constrains loading: the **ABI
version** (this section), the **SDK source version** (the `driver.hpp`
helper layer; it evolves with the project and fixes bugs without touching
the ABI), and the **yaddnsc application version**.

The host is the provider. It loads a plugin whose major matches and whose
minor is no higher than the host's, and a plugin accepts a services table on
the same rule. A plugin's minor is the minimum host minor it requires. One
plugin binary serves exactly one ABI major: the exported entry-point symbol
names are fixed, so a single `.so` cannot offer v1 and a future v2 side by
side.

Within a major, fields are only appended to the versioned structs. New host
callbacks, capability bits, status codes, log levels, and HTTP methods are
minor bumps. Moving or removing a field, growing a leaf type, or adding a
required entry point is a major bump. `YADDNSC_*_MIN_SIZE` is the ABI 1.0
baseline and stays there when a later minor appends a field. A different
major, or a plugin that requires a newer minor, is rejected at load time.
Never bypass the version check: a minor rejection means the host is older
than the driver requires (upgrade yaddnsc); a major rejection means the
driver was built for a different ABI generation.

This C ABI is new in this release. Drivers written against the old C++ plugin
API are completely incompatible and cannot load at all — they must be rebuilt
against the new SDK.

Supported platforms for this ABI are Linux ELF x86_64, Linux ELF AArch64, and
macOS 64-bit. The host loads plugins with `dlopen` (`RTLD_NOW | RTLD_LOCAL`).
The layout is enforced by the header, not just documented:
`sizeof(void*) == 8`, `sizeof(size_t) == 8`, 8-byte function pointers, and
default struct packing are `static_assert`ed at compile time, and a big-endian
toolchain fails with an `#error`. Windows is not a supported host.

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

ABI 1.0 is frozen. `test/plugin/abi_baseline/v1_0/yaddnsc/sdk/driver_abi.h`
is a pinned copy of the header, checked by sha256 in CI, and it is the
reference the freeze promise is tested against. Changing the ABI is a
deliberate act, not an edit — see
[`development.md`](development.md#plugin-abi-changes). `test/plugin/fixtures/`
holds the rejection fixtures for these rules and for the entry-point and
lifecycle contracts below (`missing_symbol`, `newer_minor`, `bad_magic`,
`short_descriptor`, `small_descriptor`, `no_validate`, `validate_http`,
`leaky_create`, `throwing_destroy`); each one is the executable form of a
rule stated in this guide.

### Struct layout and `struct_size`

The versioned top-level structs carry their size in bytes as the first field,
`struct_size`: `yaddnsc_error`, `yaddnsc_http_request`,
`yaddnsc_http_response`, `yaddnsc_update_request`,
`yaddnsc_driver_descriptor`, and `yaddnsc_host_services`.

The side that reads a struct is always the one that interprets `struct_size`,
so who sets it depends on the direction of the call:

- for a struct the caller passes in — `yaddnsc_http_request`, `yaddnsc_error`,
  `yaddnsc_http_response`, `yaddnsc_host_services`, `yaddnsc_update_request` —
  the caller sets `struct_size` to the capacity it actually provides;
- `yaddnsc_driver_descriptor` goes the other way. It is returned by the plugin
  from static storage, so the **provider — your plugin — sets `struct_size` to
  the real `sizeof(yaddnsc_driver_descriptor)`**, and every string view in it
  must point at static storage that outlives the call, not a temporary or a
  heap buffer. `YADDNSC_DEFINE_DRIVER` builds it as a `constexpr` object whose
  views are literals or constexpr character arrays. The host only reads the size and copies the
  fields out; it never writes back into the descriptor.

Leaf types do not have `struct_size` and are not prefix-compatible:
`yaddnsc_string`, `yaddnsc_bytes`, `yaddnsc_source_location`, and
`yaddnsc_http_header`. Do not append fields to them.

For an extensible struct (only descriptor/Services carry major/minor; the other
structures use `struct_size` and their enclosing protocol version):

- a value below the ABI 1.0 baseline — the `YADDNSC_*_MIN_SIZE` constants in
  `driver_abi.h` — is rejected for required inputs/outputs after protocol/version validation;
  optional `out_error` below that baseline is left unchanged;
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

Inputs and log arguments are borrowed only during the receiving call; copy
before returning if retaining them. Returned error messages remain valid after
return until the next ABI entry/service callback on the same thread, instance
destruction, or module unload, whichever happens first: copy before that event.
HTTP response views survive exchanges until update returns. Descriptor views
survive while the module is loaded. Services/context are usable from create
through matching destroy on the cycle's worker and must not cross cycles.

An HTTP request body uses `data == NULL` as a presence bit:

| `data` | `size` | Meaning |
| --- | --- | --- |
| `NULL` | 0 | no body |
| non-`NULL` | 0 | a body of length 0 |
| non-`NULL` | N | N bytes |
| `NULL` | N | invalid |

The C++ helper matches this with `std::nullopt` (no body) and an engaged
`std::string`, including an empty one (a body is present).

Both directions validate completely, and the two are not the same check:

- **The host checks the plugin.** The descriptor (non-empty name, well-formed
  views, supported capability bits), every request passed to `http_exchange`
  (non-empty URL, known method, header array and per-header views, content
  type, body), and every result the plugin returns: a status outside the ABI
  vocabulary, an error report below the ABI 1.0 baseline, or an error report
  whose `status` disagrees with the status returned, all surface as an
  "invalid … ABI error report" failure rather than something trusted.
- **The SDK checks the host.** It validates the services table it receives,
  plus every host response and error report, before exposing them to driver
  code: an unknown status becomes "host returned an unknown status" and a
  malformed report "host returned an invalid error report".

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

Every host-service exchange runs under a wall-clock budget (5s), covering
dispatch, loop I/O and worker waiting. Expiry returns `NETWORK_ERROR` with
"the API server did not respond in time" and cancels any retiring loop-side
request. A cycle cancellation or closed group returns `CANCELLED`. Neither
status by itself proves host-global shutdown; `is_cancelled()` observes this
cycle's cancellation state.

Returning `YADDNSC_STATUS_CANCELLED` from `yaddnsc_driver_update()` stays
meaningful only while the operation really is cancelled (`is_cancelled()`
non-zero) — but the host does not use your view to decide propagation. It
compares the cancel scope of the update call: a cancelled scope (shutdown, or
an outer budget) propagates and unwinds, while a cancellation returned on a
live scope is logged and counted as one failed update — the subdomain task
keeps running and retries on the next interval. Report what you observed, not
what you assume.

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

The firewall is a backstop, not a portability promise: it assumes the plugin
shares an ABI-compatible C++ runtime with the host. An exception escaping
from a plugin built with an incompatible runtime is undefined behaviour
before the firewall can run. SDK-built plugins never rely on it — the
`YADDNSC_DEFINE_DRIVER` entry points forward to `noexcept` helpers that
catch everything.

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
fails because it cannot confirm `driver_params`. `YADDNSC_DEFINE_DRIVER`
always exports the entry. `Driver::validate()` is pure virtual: each driver
must explicitly validate its schema and semantics; parameterless drivers may
explicitly return success.

`yaddnsc_driver_validate` lets `yaddnsc config test` check a driver's
`driver_params` against the driver's own schema without performing an update.
It is called on a fresh instance between create and destroy, must not perform
HTTP exchanges, and returns
`YADDNSC_STATUS_INVALID_CONFIG` with a human-readable message when the
parameter is unacceptable. The SDK's C++ helper layer emits it automatically
from `YADDNSC_DEFINE_DRIVER`; override `Driver::validate()` to add the
schema and semantic checks. See the [changelog](../CHANGELOG.md) for source
and behavior changes. Bundled drivers reuse their checked parser in
validate and update, rejecting missing/wrong-type/null/empty required strings
before HTTP. Supplied TTL values cannot be negative.

## Recommended build

Third-party drivers build against the installed SDK — the `yaddnsc-dev`
package when installing from DEB — no host sources needed:

```cmake
find_package(yaddnsc CONFIG REQUIRED COMPONENTS plugin_sdk)  # + plugin_sdk_xml / plugin_sdk_crypto if needed

add_library(my_driver MODULE my_driver.cpp)
target_link_libraries(my_driver PRIVATE yaddnsc::plugin_sdk)
```

`plugin_sdk` is the default component and requires **exactly Glaze v7.8.2**
(`find_dependency(glaze 7.8.2 EXACT CONFIG)`). `yaddnsc-dev` bundles that release,
including headers and package metadata. CMake skips incompatible candidates in
earlier prefixes and can select the bundled copy. If no exact copy is available,
configure fails with the expected version and instructions to check
`CMAKE_PREFIX_PATH`, `glaze_DIR` and stale caches.

The SDK propagates include paths, C++23 and necessary dependencies. Consumers
own their warnings and `-Werror` policy. Set visibility on the plugin itself:

```cmake
set_target_properties(my_driver PROPERTIES PREFIX "" OUTPUT_NAME "my_driver"
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
```

Export each `yaddnsc_driver_*` entry with default visibility:
`YADDNSC_DEFINE_DRIVER` handles this for C++, while C plugins use their own
`__attribute__((visibility("default")))`. First-party drivers still compile
with `-Wall -Wextra -Wpedantic -Werror` privately.

Pure C11 plugins use `find_package(yaddnsc CONFIG REQUIRED COMPONENTS plugin_abi)`
and link `yaddnsc::plugin_abi`. This target has neither Glaze nor a C++ language
requirement. The installed examples include an actual `.c` module.

Drivers that parse XML responses link the separate `plugin_sdk_xml`
component instead: `yaddnsc::plugin_sdk_xml` implies `yaddnsc::plugin_sdk`
and adds libxml2 (`find_dependency(LibXml2)`). `driver.hpp` itself never
includes libxml2, so only drivers that include `yaddnsc/sdk/xml_raii.hpp`
need this component, and it exists only in packages built with libxml2
available.

Drivers that sign requests add the `plugin_sdk_crypto` package component
and link `yaddnsc::plugin_sdk_crypto` (HMAC/SHA/hex/base64):

```cmake
find_package(yaddnsc CONFIG REQUIRED COMPONENTS plugin_sdk plugin_sdk_crypto)
target_link_libraries(my_driver PRIVATE yaddnsc::plugin_sdk yaddnsc::plugin_sdk_crypto)
```

Include `<yaddnsc/sdk/signing.hpp>` and use `yaddnsc::sdk::signing`
(`Signing` remains a source compatibility alias). The crypto target carries the
SDK include directory, C++23 and `OpenSSL::Crypto`, and emits no library.
Crypto-only consumers do not need Glaze. Request `plugin_sdk` as well when using
`driver.hpp`. Consumers compile signing with their own toolchain and standard
library ABI; OpenSSL 3.0+ remains a compile/link dependency. Signing algorithms
and the existing empty-result failure behavior are unchanged in this migration.

XML types live in `yaddnsc::sdk::xml`; the `xml_raii` alias remains available.
`OPTIONAL_COMPONENTS plugin_sdk_xml plugin_sdk_crypto` leaves the package usable
when their dependencies are missing, marking just those components unavailable.
Requesting them with `COMPONENTS` makes them required. `driver.hpp` remains the
combined C++ HTTP/logging/JSON entry point; splitting it further is deferred
until a non-JSON C++ consumer needs it.

When developing inside the yaddnsc source tree (for example for a driver that
will be bundled), link the in-tree target instead and rebuild the project:

```cmake
add_library(<name> MODULE <name>.cpp)
target_link_libraries(<name> PRIVATE yaddnsc_plugin_sdk)
```

External and released drivers should always build against the installed
package, not the source tree.

## SDK distribution

The SDK can be installed independently of the host so third-party drivers can build
without the host sources. DEB packaging splits these files into two packages:
`yaddnsc` (binary, bundled drivers, config, systemd unit, shell completions)
and `yaddnsc-dev` (everything else below):

- headers: `<prefix>/include/yaddnsc/sdk/` (the C ABI + C++ helper layer) and
  `<prefix>/include/yaddnsc/util/` (shared string/format/URL utilities);
- CMake package: `${CMAKE_INSTALL_LIBDIR}/cmake/yaddnsc` under the prefix
  (normally `<prefix>/lib/cmake/yaddnsc/`), exporting four components —
  `yaddnsc::plugin_abi`, `yaddnsc::plugin_sdk` (the default), `yaddnsc::plugin_sdk_xml`, and
  `yaddnsc::plugin_sdk_crypto`;
- crypto helpers: header-only `<prefix>/include/yaddnsc/sdk/signing.hpp`,
  compiled by the consumer and linked to `OpenSSL::Crypto` through the optional
  `yaddnsc::plugin_sdk_crypto` interface target; no C++ archive is distributed;
- standalone examples: `<prefix>/share/yaddnsc/examples` (C, HTTP, XML, crypto,
  a loader and installation checks). The development DEB does not depend on
  the same-version host runtime, and can be installed on SDK-only build machines;
- bundled drivers: `${CMAKE_INSTALL_LIBDIR}/yaddnsc/drivers` (normally
  `<prefix>/lib/yaddnsc/drivers/`). That path is also the compiled-in default
  `driver_dir` (`YADDNSC_DEFAULT_DRIVER_DIR`), so an installed driver is found
  without any configuration.

The set of bundled drivers is an explicit, auditable list —
`YADDNSC_DRIVERS` in `cmake/Drivers.cmake`, the single source of truth shared
with the driver tests. `driver/CMakeLists.txt` includes that file and loops
over it; it does not hold the list itself. Adding a bundled driver takes two
changes: the name in `YADDNSC_DRIVERS`, and a `driver/<name>/` directory with
a `CMakeLists.txt` that creates a target of the same name — the loop skips a
subdirectory that defines no target (an unsatisfied optional dependency). It
sets the `PREFIX ""` output name, warning flags, LTO, and the install rule
above. Runtime auto-discovery (skipping foreign libraries in the driver
directory) is unaffected.

Standalone consumer examples that build against the installed package live in
`test/sdk_consumer/` (`c_abi_driver.c`, `http_driver.cpp`, `xml_driver.cpp`,
`crypto_driver.cpp`).

## Driver responsibilities

A driver:

- subclasses `yaddnsc::sdk::Driver` and implements `update(UpdateContext &)`;
- parses its configuration with `parse_config<T>()` from `driver_params` JSON —
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
  `YADDNSC_DEFINE_DRIVER(YourDriver, "<name>", "<description>", "<author>", "<version>", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA, 0)`.
  The final argument is the minimum host ABI minor actually required; it must
  be supported by this SDK. Descriptor strings must be constexpr NUL-terminated
  character arrays (including literals), never pointers.

Each update call runs on a fresh driver instance (`create → update →
destroy`), so instance state never leaks between updates. **Different
instances of the same module may update concurrently** (one subdomain per
task), while a single instance is never used concurrently — keep instance
state per-call and module state thread-safe. `create()` is not process-level
one-time initialization.

The host runs the synchronous C ABI off its event loop: the whole
`create → update → destroy` cycle executes on a coroutine offload-pool
worker, one per concurrent cycle. Host HTTP is the exception — `http_exchange`
blocks that worker while the actual request runs on the loop through a bridge,
so an exchange costs a thread hop and is bounded by the wait budget below.
That split does not change what a driver may see: the plugin still runs
entirely on one thread inside one entry-point call, so the rule below holds.

The host retains a cycle owner until the worker and all HTTP children really
finish. Cancelling the awaiting caller prevents later HTTP admission and cancels
existing exchanges, while create/update/destroy remain on the same worker. A
worker waiting for HTTP continues to occupy its capacity under the synchronous
v1 contract. A queued cycle cancelled before admission never enters the plugin.
Host defects that arrive after caller cancellation remain observable at the root
error boundary. The host cannot forcibly terminate an uncooperative plugin.

The services table may be stored on the instance from `create` until
`destroy`. Do not hand it to another thread, and do not use it after
`destroy` returns. Do not cache response views past the return of
`yaddnsc_driver_update()`. Host callbacks do not re-enter plugin entry
points. `http_exchange` may be called only from `update`, on that call's
thread. `create`, `destroy`, and `validate` must not perform an exchange.

Each cycle owns its own context. Its exchange callback checks the current
phase through that context, including when the plugin saved a copy of the
Services table: only update permits HTTP. Create, validate and destroy return
`INVALID_ARGUMENT` without touching the network. Keeping a previous cycle's
context is still forbidden; calling such a dangling pointer is a use-after-free,
which a phase check cannot make safe.

So `create` must not assume an exchange is available, and must not require
HTTP to finish setting up the instance.

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
(`CORE_LOG`, `core_logger`, `uri.h`, `fmt.hpp`, `string_util.hpp`, host
`HttpClient`, `form_encode.h`, `xml_raii.h`, or anything under `spdlog/`);
the SDK surface plus privately linked third-party libraries is the whole
contract. Logging is therefore Host-Services-only — there is no host logger to
call directly, and the architecture guard rejects the include at build time.
The same guard bans `yaddnsc/util/*` directly: bundled drivers include the
`yaddnsc/sdk/*` forwarding headers only, even though those headers wrap the
shared utilities in `yaddnsc/util/*`.

That SDK surface is small and complete: `driver_abi.h` (the C ABI),
`driver.hpp` (the C++ helper layer), `string_util.hpp`, `format.hpp`,
`url_encode.hpp` (percent-encoding), `form_encode.hpp` (form encoding), and
`redact.hpp` (pulled in by `driver.hpp`; the redaction behind the log
helpers), `signing.hpp` for crypto, `version.hpp` for source version checks,
and `xml_raii.hpp` for XML drivers. Use them instead of copying
implementations into the driver. Use the bundled
`driver/cloudflare/` as the reference implementation and keep
provider-specific credentials in `driver_params`.
Do not put credentials in source code or log messages — the SDK log helpers
redact sensitive request fields by default. Payload bodies additionally carry
a fixed log budget: pass them through `redact::truncate_for_log` (declared in
`redact.hpp`) so an oversized upstream response cannot flood the log.

Drivers that need request signing can link the optional
`yaddnsc::plugin_sdk_crypto` component — header-only signing helpers
(HMAC/SHA/hex/base64) linked to OpenSSL.

## Standalone shared library

Standalone builds are discouraged. If unavoidable, the driver must be built as
a `MODULE` library with position-independent code against the SDK source
version it needs, declaring its minimum runtime ABI requirement and exporting
only the
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

`http_exchange` returns non-OK for transport failure (`NETWORK_ERROR`),
cancellation (`CANCELLED`, see above), and host-side defects
(`INTERNAL_ERROR` — a bug or an allocation failure inside the host's exchange
path). A provider status, including 4xx and 5xx and integers outside 100–599,
arrives as `STATUS_OK` plus `out_response->status_code`. The SDK does not
range-check that integer. The body is binary-safe. Response headers may repeat
and keep the casing the peer sent. Compare header names case-insensitively.
Trailers are not exposed.

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
the response after redirects. `Host`, `Content-Length`, and `Content-Type`
are rebuilt on every hop. **A cross-origin redirect — any change of scheme,
host, or port — additionally strips `Authorization`, `Cookie`, and
`Proxy-Authorization`.** A driver that authenticates with a bearer header
against a provider that redirects to another host or port therefore sees an
unauthenticated retry; expect a 401/403 on the second request and check the
status code rather than assuming the follow-up carried your credentials.

Three deadlines bound the wire time:

- 5s per host-service exchange. Expiry is `NETWORK_ERROR` with "timed out
  after 5000ms" — it is the host's budget for the whole exchange, not
  re-sent per redirect hop.
- 5s to connect per resolved address. An expired connect budget moves on to
  the next address instead of failing the exchange, so a multi-address host
  can take longer than 5s to fail.
- 30s for a whole update cycle. When it expires, the subdomain task waits the
  normal interval and tries again; it does not tear anything down.

One exchange is limited to 64 KiB of headers and 16 MiB of body.

## Troubleshooting

- `Driver not found`: check `driver_dir`, the file name, and installation.
  The default is `${CMAKE_INSTALL_LIBDIR}/yaddnsc/drivers`, where `make
  install` puts bundled drivers — a driver built in the build tree has to be
  copied there or pointed at explicitly.
- Magic mismatch, a different ABI major, or a plugin minor newer than the
  host: rebuild the driver with the current SDK headers. The host pins the
  layout and refuses a module built for a different ABI generation.
- `http_exchange` returns `NETWORK_ERROR` with "the API server did not respond
  in time": the exchange budget expired, including time waiting for loop
  dispatch. This fails the exchange; it does not imply host shutdown.
- Missing required parameters: compare `driver_params` with the provider entry
  in [`DRIVERS.md`](../DRIVERS.md).

The SDK headers in `include/yaddnsc/sdk/` are the source of truth for the
plugin interface; this guide intentionally avoids duplicating their full API
documentation.

## SDK source API and migration notes

`<yaddnsc/sdk/version.hpp>` exposes `YADDNSC_SDK_VERSION` and its major/minor/patch
macros, initially matching the application version. The CMake package version
selects source compatibility; the descriptor's ABI version selects runtime
compatibility. Only the C ABI is a stable binary interface. Installed headers
under `yaddnsc/sdk` and `yaddnsc/util` are supported source APIs; their documented
namespace aliases do not create separate implementations. CMake is the formal
build entry point; pkg-config is deferred until a consumer needs it.

Source changes for this SDK revision:

- Implement `Driver::validate()` explicitly and reuse update-time parsing plus
  semantic checks. It no longer defaults to successful validation.
- Add the minimum ABI minor (currently `0`) to `YADDNSC_DEFINE_DRIVER`.
  Literals/constexpr arrays work; pointer descriptor strings fail compilation.
- Replace `<signing.h>` and the old crypto component/target with
  `<yaddnsc/sdk/signing.hpp>` and `plugin_sdk_crypto` / `yaddnsc::plugin_sdk_crypto`.
  There is no prebuilt signing archive.
- Optional construction as `YourDriver(Services)` enables initialization logs;
  the zero-argument constructor remains supported. HTTP stays unavailable then.
- `run_update` accepts capturing response checkers returning `Result` and preserves
  their detailed errors; existing bool checkers retain HTTP-status classification.
- Named formatting scans the original template only. `{{`/`}}` escape braces,
  unknown/unclosed placeholders are preserved, and the last duplicate name wins.
- Logging macros protect service lookup, argument evaluation, formatting and
  callbacks from exceptions, and skip arguments if no logger exists. No runtime
  log-level query is added in ABI 1.0.
- Request logs include a redacted URL. JSON bodies are parsed recursively;
  malformed/unrecognised content is hidden. Form/query values end only at `&`;
  encoded keys are classified after decoding. Standard sensitive headers remain
  protected. Arbitrary provider log strings still require author review.

The plugin C ABI remains 1.0, with unchanged layout and entry points. Sensitive
field metadata and a fail-closed default for old plugins' `config show` output
are future ABI work, not implemented by this revision.
