# Plugin ABI (v1)

This document is the contract between the host and a runtime-loaded driver: the
single source of truth for the v1 plugin ABI rules. The normative form is the
header `include/yaddnsc/sdk/driver_abi.h`; this document states the same rules
for reviewers and driver authors. The driver-authoring guide is
[`custom-drivers.md`](custom-drivers.md); the process for changing the ABI is
[`development.md`](development.md#plugin-abi-changes).

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

The current ABI version is **1.1** (`YADDNSC_DRIVER_ABI_MAJOR` 1,
`YADDNSC_DRIVER_ABI_MINOR` 1), and **the ABI 1.0 layouts are stable since the
1.0.0 release**: a driver built against ABI 1.0 loads and runs on every host
implementing ABI 1.x. Those 1.0 types were frozen at 1.0.0-alpha.5, so drivers
built against alpha.5 or later already implement ABI 1.0; drivers built against
alpha.1–alpha.4 predate the freeze and must be recompiled.
Minor 1 added two things and moved no existing offset: the async execution group
and an optional log-level query on the host services table.
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
required entry point is a major bump. A `YADDNSC_*_MIN_SIZE` constant is
frozen at the baseline of the minor that introduced its type and is never
retargeted: the 1.0 types keep `YADDNSC_ERROR_MIN_SIZE`,
`YADDNSC_HTTP_REQUEST_MIN_SIZE`, `YADDNSC_HTTP_RESPONSE_MIN_SIZE`,
`YADDNSC_UPDATE_REQUEST_MIN_SIZE` and `YADDNSC_HOST_SERVICES_MIN_SIZE`, while
minor 1 added `YADDNSC_HOST_SERVICES_V1_1_MIN_SIZE`,
`YADDNSC_ASYNC_SERVICES_MIN_SIZE` and `YADDNSC_EVENT_MIN_SIZE` for the types
it introduced. A different major, or a plugin that requires a newer minor, is
rejected at load time.
Never bypass the version check: a minor rejection means the host is older
than the driver requires (upgrade yaddnsc); a major rejection means the
driver was built for a different ABI generation.

Drivers written against the old C++ plugin API, predating this C ABI, are
completely incompatible and cannot load at all; they must be rebuilt against
the current SDK.

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

1. the driver exports `yaddnsc_driver_get_descriptor` — without it no version
   can be read, and the rejection is a plain missing-symbol one;
2. the descriptor's `struct_size` covers the 8-byte version prefix
   (`struct_size`, `abi_major`, `abi_minor`);
3. `abi_major` is a major the host implements, and the host's minor is at
   least the plugin's minor. This handshake runs before the entry-point
   groups are checked, so a driver that requires a newer minor is always
   told to upgrade yaddnsc, never misreported as missing entry points;
4. `struct_size` covers that minor's baseline — the ABI 1.0 baseline for minor 0,
   the 1.1 baseline for minor 1;
5. the descriptor's `magic` identifies a yaddnsc driver;
6. the descriptor's string views and capability bits are valid. The version
   check runs before the capability check;
7. the driver exports one complete execution group, and exactly one is
   selected (see [entry points](custom-drivers.md#entry-points)).
   A module exporting symbols from both groups is rejected.

A rejection names its remedy: a major the host does not implement takes
`use a driver built for ABI v1.`, as do a too-small `struct_size` and
missing entry points, while a plugin needing a newer minor takes
`upgrade yaddnsc.`

The ABI 1.0 types are frozen.
`test/plugin/abi_baseline/v1_0/yaddnsc/sdk/driver_abi.h`
is a pinned copy of the header, checked by sha256 in CI, and it is the
reference the freeze promise is tested against; minor 1 only appends. Changing
the ABI is a deliberate act, not an edit — see
[`development.md`](development.md#plugin-abi-changes). `test/plugin/fixtures/`
holds the rejection fixtures for these rules and for the entry-point and
lifecycle contracts below (`missing_symbol`, `newer_minor`, `bad_magic`,
`short_descriptor`, `small_descriptor`, `no_validate`, `validate_http`,
`leaky_create`, `throwing_destroy`, `loader_mode_fixture`, and the async set);
each one is the executable form of a rule stated in this document or the
driver guide.

### Struct layout and `struct_size`

The versioned top-level structs carry their size in bytes as the first field,
`struct_size`: `yaddnsc_error`, `yaddnsc_http_request`,
`yaddnsc_http_response`, `yaddnsc_update_request`, `yaddnsc_driver_descriptor`,
`yaddnsc_host_services`, and the types minor 1 added —
`yaddnsc_host_services_v1_1`, `yaddnsc_async_services` and `yaddnsc_event`.

The side that reads a struct is always the one that interprets `struct_size`,
so who sets it depends on the direction of the call:

- for a struct the caller passes in — `yaddnsc_http_request`, `yaddnsc_error`,
  `yaddnsc_http_response`, `yaddnsc_host_services`, `yaddnsc_update_request`,
  and in async mode `yaddnsc_async_services` and `yaddnsc_event` —
  the caller sets `struct_size` to the capacity it actually provides;
- `yaddnsc_driver_descriptor` goes the other way. It is returned by the plugin
  from static storage, so the **provider — your plugin — sets `struct_size` to
  the real `sizeof(yaddnsc_driver_descriptor)`**, and every string view in it
  must point at static storage that outlives the call, not a temporary or a
  heap buffer. The SDK driver macros build it as a `constexpr` object whose
  views are literals or constexpr character arrays. The host only reads the size
  and copies the
  fields out; it never writes back into the descriptor.

Leaf types do not have `struct_size` and are not prefix-compatible:
`yaddnsc_string`, `yaddnsc_bytes`, `yaddnsc_source_location`, and
`yaddnsc_http_header`. Do not append fields to them.

For an extensible struct (only descriptor/Services carry major/minor; the other
structures use `struct_size` and their enclosing protocol version):

- a value below the baseline of the minor that introduced that type — the
  matching `YADDNSC_*_MIN_SIZE` constant in `driver_abi.h` — is rejected for
  required inputs/outputs after protocol/version validation;
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
  views, supported capability bits), every request passed to `http_start`
  (non-empty URL, known method, header array and per-header views, content
  type, body), and every result the plugin returns: a status outside the ABI
  vocabulary, an error report below the type's baseline, or an error report
  whose `status` disagrees with the status returned, all surface as an
  "invalid … ABI error report" failure rather than something trusted.
- **The SDK checks the host.** It validates the services table it receives,
  plus every host response and error report, before exposing them to driver
  code: an unknown status becomes "host returned an unknown status" and a
  malformed report "host returned an invalid error report".

In async mode the SDK copies the response out of the host event before your
code resumes, so no borrowed view outlives the `co_await`. The synchronous
`HttpResponse` is different: its views are host-owned and stay valid only until
`yaddnsc_driver_update()` returns.

### Capability gate

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
request. A cycle cancellation or closed nursery ingress returns `CANCELLED`.
Ingress closes when the cycle control body returns, before retiring HTTP is
fully drained; late posted requests therefore cannot extend that cycle. The
whole cycle, including work/HTTP drain, outcome publication and subscriber
acknowledgement, stays shielded from ancestor cancellation while an observer
translates it into an explicit cycle cancellation request. Neither status by
itself proves host-global shutdown; `is_cancelled()` observes this cycle's
cancellation state.

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
SDK driver macros' entry points forward to `noexcept` helpers that
catch everything.

Handle ownership: on `YADDNSC_STATUS_OK`, `*out_driver` is non-NULL and the
host owns the instance, including the matching `yaddnsc_driver_destroy()`
call. `OK` with a NULL handle is a contract violation; the host turns it
into `INTERNAL_ERROR` and owns nothing. On any non-OK return the plugin must
leave `*out_driver` `NULL` — a failed create owns nothing. The host clears
`*out_driver` before calling the plugin. If a plugin stores a handle and
then reports failure, the host destroys and clears that handle before
propagating the error.
