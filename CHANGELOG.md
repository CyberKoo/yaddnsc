# Changelog

All notable user-visible changes are recorded here, newest first; breaking
changes come first within a version. Detailed migration guidance lives in
[docs/custom-drivers.md](docs/custom-drivers.md) — entries link to it rather
than repeat it. The plugin C ABI stability promise is defined in that same
document. Release bodies on GitHub Releases are copied from the matching
section here.

## [1.0.0-alpha.7] - 2026-10-17

This revision keeps the plugin C ABI at **1.0** (frozen at 1.0.0-alpha.5;
entry points and struct layouts are unchanged, so plugin binaries built
against alpha.5 or later remain compatible) while revising the plugin SDK,
build packaging and diagnostics. Migration steps:
[SDK source API and migration notes](docs/custom-drivers.md#sdk-source-api-and-migration-notes).

### Breaking

- `Driver::validate()` is now pure virtual: every C++ driver must implement
  schema/semantic checks shared with update (parameterless drivers return
  success explicitly).
- `YADDNSC_DEFINE_DRIVER` takes a seventh argument — the minimum ABI minor
  the driver actually requires (`0` today). Descriptor strings must be
  literals or constexpr char arrays; pointers are rejected at compile time.
- Crypto helpers moved: include `<yaddnsc/sdk/signing.hpp>` (was `<signing.h>`)
  and link `yaddnsc::plugin_sdk_crypto` (was `yaddnsc::plugin_support_crypto`).
  Signing is header-only; no precompiled archive ships in `yaddnsc-dev`.
- The C++ SDK requires exactly Glaze v7.8.2 (bundled with `yaddnsc-dev`);
  other versions fail at configure time with a clear diagnostic.
- Host HTTP services accept exchanges only during an update, including through
  saved copies of the cycle's service table; calls from create/validate/destroy
  are rejected.

### Changed

- The SDK no longer imposes warning flags, `-Werror` or visibility settings
  on consumers; bundled drivers keep strict flags privately.
- Named-argument formatting and Simple URL templates scan the template once;
  inserted values are literal, `{{`/`}}` escape braces, unknown placeholders
  stay unchanged, duplicates resolve to the last value. (`{{a}}` with
  `a="VALUE"` used to render `{VALUE}` and now renders `{a}`.)
- `format_request` log output now includes a redacted `url=` field; JSON
  redaction parses real JSON and hides malformed content instead of emitting
  it raw. Consumers parsing request logs must adapt.
- Validation is stricter and shared between validate and update: required
  credential/identifier strings must be nonempty, TTL/`ttl_sec` cannot be
  negative, and both fail before any HTTP call. DNSPod's `global` and
  `record_line_id` are now truly optional with documented defaults.
- A null or undersized `out_error` no longer blocks an otherwise valid host
  service call.

### Added

- Pure-C plugins link `yaddnsc::plugin_abi` (no Glaze, no C++); XML and crypto
  are optional `find_package` components that degrade gracefully when their
  dependencies are missing.
- `yaddnsc-dev` installs minimal C/C++/crypto examples and an SDK version
  header, and no longer requires the exact-version host package — build-only
  machines can install just the SDK.

### Fixed

- DNSPod: an explicitly empty `record_line_id` is rejected instead of being
  sent upstream.
- JSON log redaction no longer leaks values containing separators (e.g. a
  token containing a comma).
- Logging argument evaluation can no longer throw into an update, and is
  skipped entirely when no logger is attached.
- Named-argument substitution no longer re-interprets inserted values as
  placeholders.
- The descriptor macro no longer miscomputes lengths for constexpr pointers.
- XML response buffers are released even when C++ string/vector allocation
  fails mid-conversion.
- The systemd unit in DEB packages now embeds the packaging prefix (`/usr`)
  regardless of the configure-time `CMAKE_INSTALL_PREFIX`.
- The compiled-in default driver directory in DEB packages now follows the
  packaging prefix (`/usr`) as well — a package configured without
  `-DCMAKE_INSTALL_PREFIX=/usr` no longer looks for drivers under
  `/usr/local`.

## [1.0.0-alpha.6] - 2026-10-11

### Fixed

- Builds against system spdlog older than 1.15 work again.
- Spawned-child handles remain memory-safe beyond their group's lifetime.
- Awaiting a coroutine outside `coro::run` is reported as a programming
  defect instead of failing obscurely.
- Failure log messages use a consistent single-colon reason style.

## [1.0.0-alpha.5] - 2026-10-11

This release freezes the plugin C ABI at **1.0**.

### Breaking

- Configuration: the legacy `"ip_source": "url"` spelling is rejected —
  spell it `"http"`.
- Configuration: the `"txt"` record type is rejected at parse time — use
  `a` or `aaaa`.

### Changed

- The runtime was rebuilt on a structured coroutine stack — coroutine
  TCP/TLS/UDP transports, HTTP client, DNS resolvers, and the plugin
  bridge — and the legacy synchronous stack was removed.
- Concurrent DNS racing rolls forward on each transient failure instead of
  giving up.
- Logs drain on a background thread.

### Added

- `--log-level` command-line option and a `YADDNSC_ENABLE_TRACE` build
  switch.
- Structured diagnostics for configuration parse failures (missing values,
  unexpected tokens).

### Fixed

- Redirect hops replay without leaking host-managed headers.
- IPv6 DNS server addresses are accepted in either spelling.

## [1.0.0-alpha.4] - 2026-10-01

### Breaking

- Configuration keys were standardized. Update existing configuration
  files before upgrading; the old keys are rejected as unknown fields,
  with no compatibility aliases or automatic migration:

  | Previous key | Migration |
  |---|---|
  | Top-level `driver` | Rename to `drivers`. Keep `domains[].driver` unchanged. |
  | `resolver.use_custom_server` | Rename to `resolver.use_custom_servers`. |
  | `domains[].subdomains[].driver_param` | Rename to `driver_params`; keep its contents unchanged. |
  | `resolver.address` or `resolver.ipaddress`, with `resolver.port` | Move the server into `resolver.servers`, for example `"servers": [{"address": "1.1.1.1", "port": 53}]`, and remove the direct fields. |
  | `domains[].subdomains[].ip_type` | Remove it. The record's `type` selects the address family: `a` uses IPv4 and `aaaa` uses IPv6. |

- The plugin ABI is now versioned as major.minor; plugins built against
  alpha.3 must be rebuilt.

### Changed

- Crypto helpers moved into the plugin-support component.
- Non-64-bit targets are rejected before compilation.

## [1.0.0-alpha.3] - 2026-09-27

### Breaking

- The plugin interface was replaced with a C ABI plus host services, and
  the SDK now ships as an installable package. Plugins written against the
  earlier C++ interface must be rewritten.

### Added

- Bootstrap DNS resolution replaces `getaddrinfo` for outbound endpoints,
  configured with the new top-level `bootstrap_dns` key.
- Shuffle resolver strategy and apex FQDN handling.
- A cancellable in-house HTTP stack replaces cpp-httplib, with reusable
  HTTP/1.0 connections and hardened HTTP/1.x handling.

### Changed

- Configuration validation reports every error at once, with clearer
  wording.

## [1.0.0-alpha.2] - 2026-08-05

### Removed

- The deprecated system libresolv backend; the native DNS resolver is the
  only backend.

### Security

- HTTPS server certificate verification is fail-closed by default, TLS
  peer identity is verified for IP literals, and implicit local CA trust
  was dropped.
- API credentials no longer leak into logs or configuration parse errors.
- Protocol hardening against DNS query/response spoofing, HTTP
  response-header smuggling, mDNS spoofing, malformed RDATA and oversized
  names, and malformed URI ports.

### Fixed

- Driver auto-discovery tolerates foreign or broken libraries in the
  driver directory, and drivers whose `create()` returns null are
  rejected.
- Non-positive update intervals and invalid `driver_dir` values are
  rejected as configuration errors.
- A use-after-scope in the deferred CLI callback of the `test` subcommand,
  and the system-spdlog build failure.

## [1.0.0-alpha.1] - 2026-07-24

First alpha of the 1.0 line. Highlights relative to the 0.x series:

### Added

- DNS driver plugin framework with eight providers, protocol tests, and
  signing utilities.
- Native DNS resolver promoted to the default; the system libresolv
  backend is deprecated.
- DNS-over-HTTPS and DNS-over-TLS support, and concurrent queries across
  multiple resolver servers.
- mDNS IP source for discovering LAN device addresses.
- Interactive CLI with inline diagnostic subcommands, and shell
  completions for bash, zsh, and fish.
- EDNS0 support.
