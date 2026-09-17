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
- Infrastructure and adapters: `src/config/` (JSON/Glaze), `src/dns/`,
  `src/ip_source/`, `src/network/` + `src/http_client/` (`net::transport` /
  `net::http`), `src/infrastructure/plugin/` (plugin host), `src/core/`
  (concrete port adapters: logger, clock, signal watcher, driver loader,
  network interfaces), `src/cli/` (parser + presenter).
- `src/composition/`: the composition root.
- `include/`: public headers — shared value types, the `HttpClient` port, and
  the plugin SDK (`include/yaddnsc/sdk/`).
- `driver/`: bundled provider plugins, built against the SDK only.

The textual boundary rules are policed by the `architecture_guard` ctest
(`cmake/ArchitectureGuard.cmake`).

## Plugin boundary (v1 alpha)

Drivers are runtime-loaded shared libraries talking to the host exclusively
through the **v1 alpha C ABI** (`include/yaddnsc/sdk/driver_abi.h`) plus an
optional C++ helper layer (`include/yaddnsc/sdk/driver.hpp`). No C++
exceptions, STL containers, or host objects cross the `.so` boundary.

- The host validates entry points, magic, exact `api_revision`, and minimum
  `struct_size` at load time; a mismatch rejects the plugin with a
  rebuild-with-current-SDK message.
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
