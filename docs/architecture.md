# Architecture Notes

This is a maintainer-oriented overview. It complements the user guide rather
than defining its public configuration contract.

## Runtime flow

```text
argv
  |
  v
main() -> Cli::parse -> Composition::dispatch
  |                         (async logging install; config load, plugin dlopen,
  |                          environment validation and trust-context build on
  |                          the main thread, before the loop starts)
  v
coro::run(loop, app::run_scheduler(config, services))
  |
  v
supervisor_group
  +-- subdomain_loop          one long-lived coroutine per subdomain; its
  |     |                     ordering state (interval, backoff, force-update
  |     |                     latch) lives in the frame
  |     +-- update_once       IP source -> DNS read -> decide -> driver update
  +-- signal watchers         SIGINT / SIGTERM -> cancel the work scope
```

The executable is a thin `main()` over `Cli::parse` and
`Composition::dispatch` (`src/composition/`). The composition root is the only
place concrete infrastructure is assembled; the application layer reaches it
through ports (`src/application/ports.h`).

## Concurrency & I/O model

The whole system is built on the coroutine runtime in
`src/infrastructure/coro/`; `.cache/coro_redesign.md` is the authoritative
description of that model. Its axioms in one paragraph: the loop thread does
loop work only; every coroutine belongs to a scope that joins its children; a
deadline and a shutdown are the same thing (a cancelled scope); every
resumption goes through the ready queue so stack depth stays bounded.

**Threads.** There are exactly three:

| Thread | Responsibility |
|--------|----------------|
| loop thread (`coro::run`'s caller) | all I/O, timers, coroutine resumption, bounded small computation |
| offload pool (`BS::thread_pool`, reached only through `coro::offload`) | anything that may block or burn CPU: the plugin ABI cycle, mDNS, and CPU-intensive work in general — `offload` is the runtime's `to_thread` analogue, the single documented exit from the loop |
| log drain thread (spdlog async sink) | writing log records off the loop |

**Loop internals** (`src/infrastructure/coro/loop.h`): a `poll()` file-descriptor
table, a timer heap, a ready queue, and a cross-thread inbox fed by
`Loop::post`. Worker→loop traffic and the pool's submit queue are the only
synchronization points in the runtime.

**Structured concurrency.** `coro::Task<T>` is lazy and runs inline at
`co_await`. `task_group` cancels siblings on the first failure and rethrows
after joining every child; `supervisor_group` reports a child failure through
`next()` without touching its siblings. The run root uses a supervisor group, so
one subdomain failing never stops the others.

**Cancellation is scope state, not a token.** `with_timeout`,
`with_deadline`, `with_cancel_scope` and `non_cancellable` run a body in a child
scope; every await is a checkpoint, and a cancelled scope makes it yield
`operation_canceled` as a value. There is no cancellation token and no I/O
timeout parameter anywhere: a timeout is composed at the call site
(`co_await with_timeout(d, ...)`), which is why the transport and HTTP layers
have no deadline arguments.

**Leaving the loop.** `coro::offload(fn)` runs `fn` on the pool and returns its
result through the loop; cancellation is *abandon* — the await returns a
cancellation value while the work packet finishes on its own. `coro::SerialLane`
serializes per-instance work on top of offload (one lane per driver name).

**Plugin bridge** (`src/infrastructure/plugin/bridge.h`). The plugin C ABI is
synchronous, so each update cycle runs on an offload worker; when it calls
`http_exchange`, the bridge posts a structured child coroutine into the
application's `TaskGroup` (passed in by the composition root), the loop runs the
coroutine HTTP client, and the worker blocks on a `std::promise`/`std::future`
until the loop fulfils it. That promise is the third synchronization boundary in
the system; it exists only because the ABI may not be re-entered, and the
blocked thread is a worker whose purpose is to block.

**Scheduler dissolution** (`src/application/subdomain_loop.cpp`). There is no
central scheduler queue, runner or executor: one long-lived coroutine per
subdomain sleeps, updates and repeats, carrying its ordering state (update
interval, last force-update time, retry back-off) as frame locals. A
provider-supplied `retry_after` overrides the next delay; the force-update
interval latches exactly as the legacy scheduler did. One cycle is bounded by
`UPDATE_BUDGET` (`with_timeout`), and the DNS read has its own shorter
`DNS_READ_BUDGET` so a dead resolver surfaces in seconds instead of holding the
whole cycle.

### Blocking-operation inventory

The loop thread never blocks. Everything that can block — a syscall, a file read,
a `dlopen`, a provider call — either leaves the loop through `coro::offload` or
runs before the loop starts. The remaining exceptions are deliberate and bounded:

| Blocking point | Where | Why it is allowed |
|----------------|-------|-------------------|
| `getifaddrs()` | `infrastructure/ip_source/iface_util.cpp`, on the loop thread | A bounded kernel snapshot of local interface metadata: read-only system state, no network round trip, no attacker-controlled size |
| startup file/loader I/O | `composition/bootstrap.cpp`, on the main thread before `coro::run` | Config read, static validation, plugin `dlopen`, the environment check and the trust-context build happen once, before any coroutine exists — there is no loop yet to block |
| CA discovery + `SSL_CTX_load_verify_locations` | `infrastructure/net/tls_context.cpp`, off-loop only (`TlsContext::create`) | Reads the trust store once and shares one immutable context, so a handshake never touches the filesystem. OpenSSL's lazy trust-directory / default-path loaders are banned by the architecture guard for exactly this reason |
| plugin ABI cycle (`create`/`update`/`destroy`) | offload pool, one serialized lane per driver (`coro::SerialLane`) | The C ABI is synchronous and must not run on the loop; the worker blocks on a promise until the bridge's HTTP child coroutine completes on the loop |
| log write | log drain thread (async spdlog sink) | Never blocks the caller; a full queue discards the newest record |
| CPU-intensive work | any `coro::offload` call site | `offload` is the single exit for blocking *and* CPU-bound work (the `asyncio.to_thread` analogue); the total off-loop workload is structurally bounded by the configuration |

`src/support/util/` holds generic helpers only. A blocking primitive (a TTL cache
with a single-flight mutex, a retrying sleep helper) must not sit on a loop path;
when one is needed, route the work through `coro::offload` instead.

## Layers

Dependency direction is enforced by the CMake target graph
(`yaddnsc_domain` ← `yaddnsc_application` ← infrastructure/adapters ←
`yaddnsc_composition` ← executable) and policed textually by the
`architecture_guard` ctest (`cmake/ArchitectureGuard.cmake`).

- `src/domain/`: pure rules and value types (update decision, update-task
  records, runtime config model, `DriverError`/`DriverUpdateCommand`). No I/O, threading,
  or third-party dependencies. `std::chrono` value types are permitted; the
  layer does not read the system clock.
- `src/application/`: the coroutine use cases and the retained ports.
  `ports.h` / `services.h` (the port interfaces and the `Services` bundle),
  `update_once.*`, `subdomain_loop.*`, `run_scheduler.*`, `diagnostics.*`,
  `environment_validator.*`, plus the retained ports `ports/log.h`,
  `ports/driver_catalog.h`, `ports/network_interfaces.h`. It names only domain
  types and `infrastructure/coro/` (the runtime is its substrate); no spdlog,
  Glaze, CLI11, OpenSSL, or dlopen. Logging goes through the `ports/log.h`
  facade.
- `src/infrastructure/coro/`: the coroutine runtime — loop, `Task`, structured
  scopes, cancellation combinators, sleeps, `AsyncMutex`, `offload`,
  `SerialLane`, signals. This is the only tree allowed to name threads, futures
  or a thread pool.
- `src/infrastructure/net/`: the transport and its shared codecs — TCP/TLS/UDP
  streams, the pre-built `tls_context` (off-loop trust material), `socket_addr`
  (POSIX sockaddr codec), `tls/cert_util` (CA discovery), and the coroutine HTTP
  client with the `uri` codec under `net/http/`.
- `src/infrastructure/dns/`: the DNS wire layer (`parser`, `validator`, `wire/`,
  `types.h`, `util.hpp`, `resolv_conf`) plus the coroutine resolvers
  (`classic`, `dot`, `doh`), the `dispatcher`, the `factory` and the
  `resolver_port` adapter. `dns_classic` (wire/parser/validator/resolv_conf) is
  the lower target.
- `src/infrastructure/ip_source/`: live interface enumeration (`iface_util`), the
  mDNS response filter (`mdns_response`), the coroutine sources (`iface`, `http`,
  `mdns`), the `adapter` implementing the IP-source port, and
  `system_network_interfaces` (the `NetworkInterfaces` port).
- `src/infrastructure/plugin/`: the plugin host — `shared_library`,
  `plugin_loader`, `driver_catalog`, `driver_instance` (lease), and the coroutine
  layer `bridge`, `host_services`, `driver_gateway`.
- `src/infrastructure/config/`: JSON/Glaze parsing, normalization, validation and
  the parse diagnostics below.
- `src/infrastructure/logging/`: `spdlog_logger` (the `Logger` facade) and
  `async_logging` (the async sink wiring).
- `src/cli/`: argument parsing (`parser`) and output presentation (`presenter`).
- `src/composition/`: the composition root.
- `src/support/`: internal shared helpers (fmt/string utilities, fd helpers);
  forwarding headers onto `include/yaddnsc/util/` where an equivalent public
  utility exists. Not part of the public surface.
- `include/yaddnsc/sdk/`: the plugin SDK (C ABI + C++ helper layer). Together
  with `include/yaddnsc/util/` these are the only public headers.
- `include/yaddnsc/util/`: header-only utilities shared by the host and the
  plugins (string utilities, named-argument formatting, percent-encoding). This
  is the single implementation site; `src/support/` and `include/yaddnsc/sdk/`
  headers only forward to it.
- `driver/`: bundled provider plugins, built solely against the public headers.

## Configuration parse diagnostics

`Config::Diagnostic::describe_parse_error()` is an internal facade in
`src/infrastructure/config/diagnostics/parse_diagnostic.cpp`. It assembles independent
components within `yaddnsc_config_infrastructure`:

- `error_adapter`: translates Glaze error codes into internal failure
  categories and scanner options. Glaze-specific cursor semantics stay here.
- `locator`: scans the failure prefix with local lookahead and returns
  structured paths, byte positions, token kinds, and malformed-token,
  container-boundary, and missing-value facts. It does not classify Glaze
  errors or generate prose; its only Glaze use is decoding escaped key names
  (`glz::read_json` on the key token alone).
- `schema`: derives expectations and accepted member names from the
  existing Config mappings in `parser.hpp`. It supplies facts, not messages.
- `decision`: applies classification priority to injected input,
  location, failure, and schema facts, including unambiguous key suggestions.
  It returns a structured diagnosis without querying the schema or raw input.
- `renderer`: formats that diagnosis; it does not parse input, query
  schema, or reconsider failure classification.

Shared internal vocabulary lives in `types.h`. These are ordinary
functions and value types, not public SDK APIs or injected runtime ports. The
components can be tested independently; changes to wording do not require
scanner changes, and schema representation changes do not require renderer
changes. Diagnostics do not alter configuration acceptance or recovery policy.
They may include key names and declared schema constants, but never echo input
values. Content positions are 1-based; columns count bytes since the last LF.
File-operation errors have no content position.

## Plugin boundary (v1 alpha)

Drivers are runtime-loaded shared libraries talking to the host exclusively
through the **v1 alpha C ABI** (`include/yaddnsc/sdk/driver_abi.h`) plus an
optional C++ helper layer (`include/yaddnsc/sdk/driver.hpp`). No C++
exceptions, STL containers, or host objects cross the `.so` boundary.

- The host reads the 8-byte version prefix, then accepts the plugin when
  `yaddnsc_abi_provides` says the host provides the plugin's `abi_major` and
  `abi_minor`. The ABI 1.0 baseline `struct_size` is checked after the
  version. A mismatch rejects the plugin with a rebuild-with-current-SDK
  message. Four entry points are required (`get_descriptor`, `create`,
  `destroy`, `update`); a fifth, `yaddnsc_driver_validate`, is optional
  since ABI 1.0 — the host dlsym-probes it so `config test` can check
  `driver_params` against the driver's schema. A missing entry means the
  plugin provides no such check. `config test` fails in that case; the
  plugin still loads and can update. Capability bits are enforced before
  the plugin runs: `A` and `AAAA` are delivered only when the matching bit
  is set. Any other record type is rejected as `DriverError::UPDATE_FAILED`,
  and that gate produces no plugin ABI status.
- Each update runs on a fresh driver instance (`create → update → destroy`).
  Instances of the same module may update concurrently; a single instance is
  never used concurrently. `create()` is not process-level one-time
  initialization.
- Provider HTTP, logging, and cancellation reach the plugin through Host
  Services: the host owns the HTTP client (reached through the bridge above) and
  the log sink (source location is forwarded to `spdlog::source_loc`).
  `is_cancelled()` reports the host's cancellation state for the current
  operation. See [Custom Drivers](custom-drivers.md) for the SDK guide before
  changing the interface or building a module outside the project build.

## Documentation ownership

- User-facing behaviour: root `README.md` and `README_CN.md`.
- Provider parameters: `DRIVERS.md` and `DRIVERS_CN.md`.
- Build, tests, and CI: `development.md`.
- Driver interface and ABI: `custom-drivers.md`.

When changing a public command, configuration field, install location, or
provider parameter, update its owner document in the same change.
