# Custom Drivers

This document is for developers who want to add a DNS provider driver to
yaddnsc. End users configuring one of the bundled providers should start with
[`README.md`](../README.md) and [`DRIVERS.md`](../DRIVERS.md).

## Compatibility

The ABI contract — versioning, struct layout, validation, capability,
cancellation and lifetime rules — lives in [`plugin-abi.md`](plugin-abi.md).
What a driver author needs up front:

- drivers are runtime-loaded shared libraries talking to the host through the
  v1 plugin ABI (`include/yaddnsc/sdk/driver_abi.h`, plain C11) plus an
  optional C++ helper layer (`include/yaddnsc/sdk/driver.hpp`, C++23);
- the current ABI version is **1.1**; a driver built against ABI 1.0 loads on
  every host implementing ABI 1.x. Minor 1 added the async execution group and
  an optional log-level query;
- the host checks the driver's ABI version, descriptor and capability bits at
  load time, and validates every value crossing the boundary in both
  directions;
- `driver_abi.h` is plain C11, so a driver written against that header alone
  needs no C++ at all.

## Entry points

A plugin exports `yaddnsc_driver_get_descriptor` plus **exactly one** complete
execution group. The host rejects a module that exports symbols from both
groups, and the selected mode never falls back to the other at runtime.

**Write new drivers against the async group.** Every bundled driver uses it, it
keeps HTTP waits off the offload pool, and it is the group that receives
further ABI work.

The synchronous group stays **supported**: it still loads and runs exactly as it
did in ABI 1.0, for the whole ABI 1.x line. Only a future major version could
retire it — nothing in this minor removes or degrades it, and existing drivers
keep working unchanged. It is simply the path we no longer recommend: new
features and bug fixes land on the async group first, so a new driver should
start there.

`yaddnsc_driver_validate` (sync) and `yaddnsc_driver_async_validate` (async)
are both **optional**: the host probes the one matching the selected group with
`dlsym`. When it is absent, the plugin still loads and can perform updates, and
`yaddnsc config test` fails because it cannot confirm `driver_params`. The
SDK's helper layer always exports it. `validate()` is pure virtual: each driver
must explicitly validate its schema and semantics; parameterless drivers may
explicitly return success.

The validate entry lets `yaddnsc config test` check a driver's
`driver_params` against the driver's own schema without performing an update.
It is called on a fresh instance between create and destroy, must not perform
HTTP exchanges, and returns
`YADDNSC_STATUS_INVALID_CONFIG` with a human-readable message when the
parameter is unacceptable. The SDK's C++ helper layer emits it automatically
from its driver macro; override `validate()` to add the schema and semantic
checks. See the [changelog](../CHANGELOG.md) for source
and behavior changes. Bundled drivers reuse their checked parser in
validate and update, rejecting missing/wrong-type/null/empty required strings
before HTTP. Supplied TTL values cannot be negative.

`DriverHelpers` carries that shared validation so a driver does not repeat
it: `require_non_empty(name, value)` rejects an empty required parameter and
an optional parameter that was provided empty (one message for both:
`Required parameter '<name>' must not be empty`),
`optional_non_negative(name, value)` rejects a negative optional number
(`Parameter '<name>' must not be negative`), and
`validate_via(driver_param_json, parse_params)` runs the driver's own parser
for the `validate()` entry, so validate and update reject the same documents.
The parser reports an unacceptable document by throwing `ConfigParseError`;
the ABI entry turns that into `YADDNSC_STATUS_INVALID_CONFIG` and keeps the
message.

### Async entry points (ABI 1.1, recommended)

The two groups are alternatives; a module exports one or the other:

- **async (recommended)**: `yaddnsc_driver_async_create`,
  `yaddnsc_driver_async_destroy`, `yaddnsc_driver_update_async_begin`,
  `yaddnsc_driver_update_async_resume`, optional
  `yaddnsc_driver_async_validate`;
- sync (supported, not recommended): `yaddnsc_driver_create`,
  `yaddnsc_driver_destroy`, `yaddnsc_driver_update`, optional
  `yaddnsc_driver_validate`.

A plugin whose descriptor requires minor 0 and exports a complete sync group
stays sync, even if it also exports async names; the host keeps it on the
sync path and never supplies the async table. Async mode requires a
descriptor minimum minor of at least 1 and a complete async group. A plugin
requiring minor 1 or newer may still choose the sync group: the declared
minimum alone does not select async mode. Such a plugin must export exactly
one complete group, including the matching validate entry and no symbol from
the other group. The mode is fixed at load; a failed async step never falls
back to sync update, and a failed sync entry never falls back to async.

The async table never carries the sync exchange callback, and neither table
exposes a conversion to the other. The async instance lifecycle, PENDING/event
rules and callback borrow periods are defined in
`<yaddnsc/sdk/driver_abi.h>`. This host loads and runs both modes. Sync
drivers receive the sync table; async drivers receive the async table, and the
gateway drives begin/resume while HTTP waits stay on the loop instead of
occupying a worker. The C++ async SDK
(`<yaddnsc/sdk/async_driver.hpp>`) provides `AsyncDriver`, `AsyncServices`,
`AsyncUpdateContext`, one sequential exchange awaitable, and the async driver
macro; it emits only the async group. Concurrent fan-out is not part of this
SDK revision. `co_await context.exchange(...)` returns `AsyncExchangeResult`
(`std::expected<AsyncHttpResponse, HttpError>`). `AsyncHttpResponse` owns its
`std::string` body and `std::vector<HttpHeader>` header strings, preserving
binary bytes, header order, casing and duplicates. The SDK copies the ABI
payload before business code resumes and moves the result out of the
awaitable. Copies own independent storage; moves transfer ownership, so neither
the event nor the awaitable needs to remain alive. Async response checkers must
accept `const AsyncHttpResponse&`, not `const HttpResponse&`. The host-side
half of that awaitable — the exchange host type, the mutators that deliver a
response or a cancellation, and the instance type — lives in
`<yaddnsc/sdk/detail/async_instance.hpp>`, which `async_driver.hpp` includes
last. It is the host's wiring, not driver interface: do not name it from
driver code.

`ExchangeResult` carries the three-field `HttpResponse` aggregate
(`status_code`, `headers`, `body`), whose header/body views borrow the host
arena until `yaddnsc_driver_update` returns; there is no shared owner or
conversion between the two response types. An exception from
`yaddnsc_driver_async_destroy` is a fatal host cleanup failure: the host
cannot prove the instance was released and will not release the module or
services. The bundled drivers use the async SDK and therefore require ABI
1.1; their provider configuration is unchanged.

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
`YADDNSC_DEFINE_ASYNC_DRIVER` handles this for C++, while C plugins use their own
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
- logs through the `YADDNSC_SDK_LOG_*` macros (`YADDNSC_ASYNC_SDK_LOG_*` for
  an async driver) — the call site
  (`file` / `line` / `function`) is forwarded through Host Services to the
  host logger (`spdlog::source_loc`); there is no `-rdynamic` / `CORE_LOG`
  symbol backfill;
- reports outcomes as `yaddnsc::sdk::Error` values with the appropriate
  `YADDNSC_STATUS_*` code;
- exports itself with
  `YADDNSC_DEFINE_ASYNC_DRIVER(YourDriver, "<name>", "<description>", "<author>", "<version>", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)`.
  It emits `get_descriptor` plus the complete async group and declares a
  minimum host ABI minor of 1; use `YADDNSC_DEFINE_ASYNC_DRIVER_MIN` only when a
  higher minimum is a real requirement, never below 1.
  Descriptor strings must be constexpr NUL-terminated character arrays
  (including literals), never pointers.

Each update call runs on a fresh driver instance (`create → update →
destroy`), so instance state never leaks between updates. **Different
instances of the same module may update concurrently** (one subdomain per
task), while a single instance is never used concurrently — keep instance
state per-call and module state thread-safe. `create()` is not process-level
one-time initialization.

In async mode the host runs the protocol on its event loop: each plugin entry
call is still executed on an offload-pool worker, one shielded step at a time,
but a `co_await` inside `begin` or `resume` returns `YADDNSC_STATUS_PENDING`
and releases that worker, so a pending HTTP exchange costs no thread capacity.
Accepted HTTP commands start only after the step returns. The plugin still runs
entirely on one thread inside one entry-point call, so the threading rule above
holds unchanged.

The host retains a cycle owner until the step and all HTTP tasks really
finish. Cancelling the awaiting caller prevents later HTTP admission and
cancels existing exchanges; the host then delivers one
`YADDNSC_EVENT_UPDATE_CANCELLED` event so the plugin can retire its own state. A
queued cycle cancelled before admission never enters the plugin.
Host defects that arrive after caller cancellation remain observable at the root
error boundary. The host cannot forcibly terminate an uncooperative plugin.

The services table may be stored on the instance from `create` until
`destroy`. Do not hand it to another thread, and do not use it after
`destroy` returns. Host callbacks do not re-enter plugin entry
points. `http_start` and `http_cancel` may be called only from `begin` and
`resume`, on that call's thread. `create`, `validate` and `destroy` must not
start an exchange.

Each cycle owns its own context. Its HTTP callbacks check the current
phase through that context, including when the plugin saved a copy of the
Services table: only begin and resume permit HTTP. Create, validate and
destroy return `INVALID_ARGUMENT` without touching the network. Keeping a
previous cycle's
context is still forbidden; calling such a dangling pointer is a use-after-free,
which a phase check cannot make safe.

So `create` must not assume an exchange is available, and must not require
HTTP to finish setting up the instance.

`capabilities` declares the record types the driver accepts: `A` requires
`YADDNSC_DRIVER_CAPABILITY_A`, `AAAA` requires
`YADDNSC_DRIVER_CAPABILITY_AAAA`. The host enforces the gate before the
plugin is called and rejects unknown bits at load; the full rules are in
[the capability gate](plugin-abi.md#capability-gate). A plugin that refuses
a record returns `YADDNSC_STATUS_UNSUPPORTED_RECORD`.

A plugin must not include host `src/` headers or legacy utility headers
(`CORE_LOG`, `core_logger`, `uri.h`, `fmt.hpp`, `string_util.hpp`, host
`HttpClient`, `form_encode.h`, `xml_raii.h`, or anything under `spdlog/`);
the SDK surface plus privately linked third-party libraries is the whole
contract. Logging is therefore Host-Services-only — there is no host logger to
call directly, and the architecture guard rejects the include at build time.
The same guard bans `yaddnsc/util/*` directly: bundled drivers include the
`yaddnsc/sdk/*` forwarding headers only, even though those headers wrap the
shared utilities in `yaddnsc/util/*`.

The sync `driver.hpp` and async `async_driver.hpp` headers share the DTOs and
the ABI value conversion through `driver_common.hpp`; the pieces the two modes
do not share live in their own headers. `log.hpp` defines both log macro
families once and is re-exported by both mode headers. `run_update.hpp` holds
the update-tail helpers — `format_request` and the shared response helpers
below — and pulls in `upstream_failure.hpp`
for upstream rejection classification and Retry-After hints, whose date-form
parser lives in `http_date.hpp`. `detail/driver_instance.hpp` and
`detail/async_instance.hpp` hold the host-side machinery the definition macros
expand to (the async exchange host, the awaitable mutators and the instance
types); each mode header includes its own last, so existing include paths and
macro names keep working. Those internals are not part of the interface you
write against — a driver awaits `exchange(...)` and never names them. The C ABI
is unchanged. Async-only consumers do not include synchronous entry-point
implementations.

`run_update.hpp` also carries the response skeleton every bundled driver
shares, so a driver writes only its provider-specific branches:

| Helper | Purpose |
| ------ | ------- |
| `log_response_body(services, response)` | Trace the upstream body; truncated to the log budget and formatted only when the level is enabled |
| `transient_rejection(services, response, subject)` | Log the WARN for a transient status (429 or 5xx) and return the rejection to report; `nullopt` when the status is not transient |
| `status_rejection(services, response, subject)` | Log the ERROR for a response without provider detail and return the rejection |
| `upstream_rejection(response, message)` | Build the rejection from the upstream status classification and the provider's own error message |
| `rejection_message(subject, response)` | The message for a response that carries no provider detail |

`subject` names the service in the message, for example `Cloudflare API
request`. The classification these helpers apply is defined under
[Error reports and HTTP](#error-reports-and-http).

That SDK surface is small and complete: `driver_abi.h` (the C ABI),
`driver.hpp` (the C++ helper layer), `log.hpp` (log macros), `run_update.hpp`
(update-tail helpers), `string_util.hpp`, `format.hpp`,
`url_encode.hpp` (percent-encoding), `form_encode.hpp` (form encoding), and
`redact.hpp` (pulled in by `driver.hpp`; the redaction behind the log
helpers, which decodes a form/query key through the shared
`yaddnsc/util/url_decode.hpp`), `signing.hpp` for crypto (`as_bytes` is the
borrowed byte view its primitives take, so callers do not hand-roll
`string_view`→`span` casts), `version.hpp` for
source version checks, and `xml_raii.hpp` for XML drivers. Use them instead of
copying implementations into the driver. Use the bundled
`driver/cloudflare/` as the reference implementation — it shows the shared
skeleton, the provider-specific branches and the `Result`-returning response
checker — and keep
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

`http_start` reports a non-OK status for transport failure (`NETWORK_ERROR`),
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

`AsyncDriver::run_update` classifies a response that `check_response` rejects.
401 and 403 become `AUTHENTICATION_FAILED`. 429 becomes `RATE_LIMITED`, and the
first `Retry-After` value — a delay in seconds, or an IMF-fix HTTP-date
measured from now — is copied into `retry_after_seconds`. An unparsable or
already-past date contributes no hint. Every other rejection stays
`UPSTREAM_REJECTED` with `retry_after_seconds` 0. A `check_response` that
accepts the response stays a success.

A `check_response` that returns a `Result` keeps its own error: the status it
carries and the message it reports reach the caller unchanged, so the
provider's own error detail is not lost. `upstream_rejection(response,
message)` builds that error with the classification above, and the update tail
still logs the domain context next to it. The bundled drivers all return a
`Result` and report the provider's message — the parsed error code and text —
instead of a generic rejection.

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
- `http_start` yields `NETWORK_ERROR` with "the API server did not respond
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

- **New drivers should implement the async execution group** (see
  [entry points](#entry-points)). `HttpResponse` is now purely borrowed;
  copying it does not extend its lifetime.
- Implement `validate()` explicitly and reuse update-time parsing plus
  semantic checks. It no longer defaults to successful validation.
- No driver source change is needed for the SDK header split. The logging
  macros, `format_request`, the upstream-failure helpers and the definition
  macros keep their names and their existing include paths; the pieces that
  moved into `detail/driver_instance.hpp` / `detail/async_instance.hpp` are the
  host seam and need no include of their own.
- The definition macros take the minimum host ABI minor as their last
  argument (`YADDNSC_DEFINE_ASYNC_DRIVER` already declares 1); pointer
  descriptor strings fail compilation — literals and constexpr arrays work.
- Replace `<signing.h>` and the old crypto component/target with
  `<yaddnsc/sdk/signing.hpp>` and `plugin_sdk_crypto` / `yaddnsc::plugin_sdk_crypto`.
  There is no prebuilt signing archive.
- Optional construction as `YourDriver(Services)` enables initialization logs;
  the zero-argument constructor remains supported. HTTP stays unavailable then.
- `run_update` accepts capturing response checkers returning `Result` and
  preserves their detailed errors; existing bool checkers retain HTTP-status
  classification.
- Named formatting scans the original template only. `{{`/`}}` escape braces,
  unknown/unclosed placeholders are preserved, and the last duplicate name wins.
- Logging macros protect service lookup, argument evaluation, formatting and
  callbacks from exceptions, and skip arguments if no logger exists. ABI 1.1
  adds the optional `log_enabled` query in `yaddnsc_host_services_v1_1`: the
  macros evaluate the level, consult the query, and only then evaluate the log
  arguments and format. A disabled level therefore skips argument evaluation
  and formatting, not the level expression; without a log callback neither is
  evaluated. The macros fall back to the original unconditional path when the
  host does not provide the query or the query fails.
- Request logs include a redacted URL. JSON bodies are parsed recursively;
  malformed/unrecognised content is hidden. Form/query values end only at `&`;
  encoded keys are classified after percent-decoding, and a key that decodes
  to anything but a legal key character (a malformed escape, a space, a `+`)
  hides the whole input instead. Standard sensitive headers remain
  protected. Arbitrary provider log strings still require author review.

The plugin C ABI is now 1.1 — the contract and freeze rules live in
[`plugin-abi.md`](plugin-abi.md). Sensitive field metadata and a fail-closed
default for old plugins' `config show` output are future ABI work, not
implemented by this revision.
