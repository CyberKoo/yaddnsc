# Architecture Notes

This is a maintainer-oriented overview. It complements the user guide rather
than defining its public configuration contract.

## Runtime flow

```text
JSON configuration
       |
       v
Composition root (config load, environment validation, assembly)
       |
       v
RunLifecycle -> SchedulerRunner -> UpdateWorkflow (per subdomain)
       |
       +--> IP source port: interface | HTTP(S) | mDNS
       |
       +--> DNS resolver port: UDP/TCP | DoH | DoT
       |
       v
DriverGateway -> provider driver (.so) -> provider HTTP API
```

The executable is a thin `main()` over `Cli::parse` and
`Composition::dispatch` (`src/composition/`). The composition root is the only
place concrete infrastructure is assembled; everything below it talks to
ports (`src/application/ports/`).

## Layers

Dependency direction is enforced by the CMake target graph
(`yaddnsc_domain` ← `yaddnsc_application` ← infrastructure/adapters ←
`yaddnsc_composition` ← executable):

- `src/domain/`: pure rules and value types (update decision, schedule queue,
  runtime config model). No I/O, threads, clocks, or third-party libraries.
- `src/application/`: use cases over the ports — update workflow, scheduler
  runner, run lifecycle, diagnostics, environment validation. No spdlog,
  Glaze, CLI11, OpenSSL, or dlopen; logging goes through the `ports/log.h`
  facade.
- Infrastructure and adapters: `src/infrastructure/config/` (JSON/Glaze), `src/infrastructure/dns/`,
  `src/infrastructure/ip_source/`, `src/infrastructure/network/` + `src/infrastructure/network/http/` (`net::transport` /
  `net::http`), `src/infrastructure/plugin/` (plugin host), `src/infrastructure/logging/`, `src/infrastructure/time/`, and `src/infrastructure/process/`
  (concrete port adapters: logger, clock, signal watcher, driver loader,
  network interfaces), `src/cli/` (parser + presenter).

  Within DNS infrastructure the classic resolver, wire format, parser and the
  bootstrap/`resolv.conf` helpers form a separate lower target
  (`yaddnsc_dns_classic`, depending only on domain + network infrastructure)
  so that `net::transport` can resolve hostname targets through them:
  `SocketStream` never calls `getaddrinfo` — hostname endpoints go through
  the bootstrap DNS servers (`bootstrap_dns`, else `/etc/resolv.conf`),
  keeping name resolution cancellable and inside the connect deadline. The
  DoH/DoT resolvers, dispatcher and resolver factory sit above HTTP in
  `yaddnsc_dns_infrastructure`, which would otherwise create a dependency
  cycle with the transport.
- `src/composition/`: the composition root.
- `src/support/`: internal shared helpers (fmt/string utilities, cancellation
  primitives); forwarding headers onto `include/yaddnsc/util/` where an
  equivalent public utility exists. Not part of the public surface.
- `include/yaddnsc/sdk/`: the plugin SDK (C ABI + C++ helper layer). Together
  with `include/yaddnsc/util/` these are the only public headers. All host
  implementation headers, including shared value types and the `HttpClient`
  port, live under `src/`.
- `include/yaddnsc/util/`: header-only utilities shared by the host and the
  plugins (string utilities, named-argument formatting, percent-encoding).
  This is the single implementation site — `src/support/` and
  `include/yaddnsc/sdk/` headers only forward to it, and both sides must use
  it instead of carrying local copies.
- `driver/`: bundled provider plugins, built solely against the public
  headers (`include/yaddnsc/sdk/` and `include/yaddnsc/util/`).

The textual boundary rules are policed by the `architecture_guard` ctest
(`cmake/ArchitectureGuard.cmake`).

## Plugin boundary (v1 alpha)

Drivers are runtime-loaded shared libraries talking to the host exclusively
through the **v1 alpha C ABI** (`include/yaddnsc/sdk/driver_abi.h`) plus an
optional C++ helper layer (`include/yaddnsc/sdk/driver.hpp`). No C++
exceptions, STL containers, or host objects cross the `.so` boundary.

- The host validates entry points, magic, exact `api_revision`, and minimum
  `struct_size` at load time; a mismatch rejects the plugin with a
  rebuild-with-current-SDK message. Four entry points are required
  (`get_descriptor`, `create`, `destroy`, `update`); a fifth,
  `yaddnsc_driver_validate`, is optional within api_revision 1 — the host
  dlsym-probes it so `config test` can check `driver_param` against the
  driver's schema, and skips the check when the plugin does not export it.
- Each update runs on a fresh driver instance (`create → update → destroy`).
  Instances of the same module may update concurrently; a single instance is
  never used concurrently. `create()` is not process-level one-time
  initialization.
- Provider HTTP, logging, and cancellation reach the plugin through Host
  Services; the host owns the actual HTTP client and the log sink (source
  location is forwarded to `spdlog::source_loc`).

See [Custom Drivers](custom-drivers.md) for the SDK guide before changing the
interface or building a module outside the project build.

## Documentation ownership

- User-facing behaviour: root `README.md` and `README_CN.md`.
- Provider parameters: `DRIVERS.md` and `DRIVERS_CN.md`.
- Build, tests, and CI: `development.md`.
- Driver interface and ABI: `custom-drivers.md`.

When changing a public command, configuration field, install location, or
provider parameter, update its owner document in the same change.
