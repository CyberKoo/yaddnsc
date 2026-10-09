# Architecture Notes

This is a maintainer-oriented overview. It records the boundaries a change has to
respect; the code itself remains the record of how any one module is built. It
complements the user guide rather than defining its public configuration
contract.

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
coro::run(loop, app::run_root(config, services))
  |
  v
supervisor_group
  +-- subdomain_loop          one long-lived coroutine per subdomain; its
  |     |                     ordering state (interval, backoff, force-update
  |     |                     latch) lives in the frame
  |     +-- run_update_cycle       IP source -> DNS read -> decide -> driver update
  +-- signal watchers         SIGINT / SIGTERM -> cancel the work scope
```

The executable is a thin `main()` over `Cli::parse` and
`Composition::dispatch`. The composition root is the only place concrete
infrastructure is assembled; the application layer reaches it through ports.
Each command handler owns its own presentation and exit code for its expected
failures; `main()` catches defects escaping dispatch and owns the logging
pipeline's lifetime.

## Concurrency & I/O model

The whole system is built on the coroutine runtime in
`src/infrastructure/coro/`. Its axioms: the loop thread does loop work only;
every coroutine belongs to a scope that joins its children; a deadline and a
shutdown are the same thing (a cancelled scope); every resumption goes through
the ready queue, so stack depth stays bounded.

**Threads.** There are exactly three:

| Thread | Responsibility |
|--------|----------------|
| loop thread (`coro::run`'s caller) | all I/O, timers, coroutine resumption, bounded small computation |
| offload pool (`BS::thread_pool`, reached through `coro::offload`; min(hardware cores, 4) workers, at least 2) | anything that may block or burn CPU: the plugin ABI cycle, mDNS, and CPU-intensive work in general — `offload` is the runtime's `to_thread` analogue and the single exit from the loop |
| log drain thread (spdlog async sink) | writing log records off the loop |

**Loop internals** (`src/infrastructure/coro/loop.h`): a `poll()` descriptor
table, a timer heap, a ready queue and a cross-thread inbox.

**Structured concurrency.** `coro::Task<T>` is lazy and runs inline at
`co_await`. `task_group` cancels siblings on the first failure and rethrows
after joining every child; `supervisor_group` reports a child failure through
`next()` while its siblings keep running. `spawn_discard` starts a
fire-and-forget child whose bookkeeping is shed at completion, which keeps a
process-lifetime group bounded by its live children. The run root uses a
supervisor group, so one subdomain failing leaves the others running.

Child results are single-consumer: `co_await Handle<T>` claims the result at
await entry, shared across handle copies and `next<T>()`. Joining an empty or
already claimed handle throws `std::logic_error`. `next<T>()` returns `nullopt`
as soon as there are no unclaimed children of type `T`, without waiting for
other result types. Handles are borrowed and must not outlive the group.
Keeping the body frame alive does not extend the lifetime of body locals that
have already left scope; child references must still obey ordinary C++ lifetime
rules.

Completed task frames have RAII ownership while results are moved out, including
throwing moves. Slot and wait registrations commit only after their fallible
allocations succeed. Invalid or uncatchable signals fail at registration, and
`sigaction` failures are reported rather than leaving an undeliverable wait.
`coro::run` requires a fresh loop and structured root completion. Task defects
still propagate normally. Exceptions escaping loop dispatch, or failure to post
an offload completion, call `std::terminate`: losing a dispatch batch or completion
cannot be recovered by safely unwinding parked frames. External stop before root
completion is likewise fatal. Loop construction throws if its wake pipe cannot
be created.

**Cancellation is scope state.** `with_timeout`, `with_deadline`,
`with_cancel_scope` and `non_cancellable` run a body in a child scope;
cancellable waits are checkpoints, and a cancelled scope makes them throw
`coro::Cancelled`, a control exception outside the `std::exception` hierarchy.
A timeout is composed at the call site (`co_await with_timeout(d, ...)`), which
is why the transport and HTTP layers take their deadline from the surrounding
scope rather than from an argument. Cancellation is sticky until the
responsible scope exits; a scope absorbs its own cancellation while ancestor
cancellation propagates; groups join all children before propagating it, as
scope state rather than as a child defect. Cleanup runs inside
`non_cancellable`, then lets cancellation propagate. Broad coroutine catches
rethrow `Cancelled` ahead of other failures; plugin C ABI adapters convert it to
`YADDNSC_STATUS_CANCELLED`, and ABI values a plugin returns are converted back
at the driver gateway.

### Coroutine API boundary

Application code names runtime capabilities and uses the values they return.
The implicit runtime context, loop and clock objects, coroutine frames,
registrations and scope waiter lists stay inside the runtime, which
implementations reach directly and share with each other. The application
surface is task ownership and structured groups, the cancellation combinators
and `ScopeOutcome`, cancellable waits, loop time as values, `checkpoint()`,
`current_scope()`, `offload`, `MutexGuard`, and the public forward declarations
— declared in `src/infrastructure/coro/` and enumerated by the guard allowlist
in `cmake/ArchitectureGuard.cmake`.

Shared runtime implementation types — frames, promises, awaiters, waiter and
timer nodes, result storage, group bookkeeping, and loop/scope access helpers —
live in `src/infrastructure/coro/detail/` under `coro::detail`. Public classes
may keep private helper types and state inside the class. Public template
headers include the complete definitions they need; this does not require every
private implementation type to move into `detail/`. Application code must not
include, name or access runtime internals.

`Loop` and `CancelScope` keep their scheduling, registration and waiter
bookkeeping private. `coro::detail::LoopAccess` and `coro::detail::ScopeAccess`
are the only ways in, so a caller cannot bypass the ready queue or leave a waiter
linked into a scope it does not own. The offload pool's third-party type stops
at `Loop::Pool` in `loop.cpp`, so including a task no longer compiles a thread
pool.

**Header layering inside the runtime.** `detail/` is a namespace and directory
of this module, not a layer of its own: a detail header implements the public
type beside it and may include it, and a public header includes the detail
headers it needs. What is not allowed is a cycle between the two directions.
Awaiting a member function requires that member's awaitable to be complete at
the call site, so `async_mutex.hpp` includes the header defining its awaiter and
that header must not include `async_mutex.hpp` — which is why `MutexGuard` and
the shared `MutexState` are top-level types rather than nested members. The
include graph is verified to be acyclic across `src/`.

`Loop` and `Clock` remain interfaces for composition, infrastructure and runtime
tests, not application APIs. Composition and infrastructure keep their startup,
I/O and runtime-internal access; runtime tests construct `Loop` and `ManualClock`
directly. Composition owns loop creation and the `coro::run` entry point. The
guard's coroutine header allowlist and internal-access checks apply only to
`src/application/`, not to composition, infrastructure or tests.

A scope an application holds is always the one a combinator passed to its body,
`TaskGroup::scope()`, or the enclosing scope reached with `co_await
coro::current_scope()`. It is borrowed for the lifetime of that combinator or
group and used on the loop thread. Reading it is not a checkpoint; cancelling it
(`cancel()`) and re-observing cancellation (`throw_if_cancelled()`) are explicit
calls, and the latter is how the plugin gateway turns a synchronous ABI abort
into coroutine cancellation. The run root stores that borrow to cancel its work
from a sibling signal watcher, and clears it before the group scope is
destroyed.

**Checkpoints.** `checkpoint()` yields and checks cancellation explicitly; use
it in CPU loops. Sleep, fd waits, signal waits, mutex acquisition, offload and
transport I/O are checkpoints too. Awaiting a `Task` or `Handle` propagates its
result; `current_time()` is a plain value read that returns immediately,
leaving cancellation to the enclosing await.

**Leaving the loop.** `coro::offload(fn)` runs `fn` on the pool and returns its
result through the loop. Cancellation is *abandon*: the await throws
`Cancelled` while the work packet finishes on its own. `offload` takes ownership
of its callable immediately, so a task created from a temporary callable may be
stored before it is awaited. Callable captures must own their data or refer to
objects that outlive the worker, even after cancellation. Loop destruction joins
workers before closing the wake pipe or destroying the inbox; an indefinitely
blocked worker therefore also blocks loop destruction.

**Plugin bridge.** The plugin C ABI is synchronous, so an update cycle runs on
an offload worker. When the plugin calls `http_exchange`, the bridge posts a
fire-and-forget child coroutine into the application's `TaskGroup` (supplied by
the composition root); the loop runs the coroutine HTTP client and the worker
waits for it. Abandoning one update cancels that exchange's own scope, so the
plugin's HTTP stops at once.

**Subdomain loops.** Each subdomain is one long-lived coroutine that sleeps,
updates and repeats, carrying its ordering state — update interval, last
force-update time, retry back-off — as frame locals, and reusing one
`UpdateTask` across cycles. A provider-supplied `retry_after` overrides the next
delay and the force-update interval latches. Each cycle is bounded by its own
timeout, with a shorter one around the DNS read, so a dead resolver surfaces in
seconds. A subdomain loop returns under cancellation; if every loop dies of a
defect with no shutdown requested, the run root exits with a failure status and
lets the supervisor (`Restart=on-failure`) start the daemon again.

### Blocking-operation inventory

The loop thread stays non-blocking. Everything that can block — a syscall, a
file read, a `dlopen`, a provider call — reaches the system through
`coro::offload` or during startup, before the loop starts. A few points sit on a
specific thread by design, each bounded:

| Blocking point | Where | Why it is bounded |
|----------------|-------|-------------------|
| `getifaddrs()` | `infrastructure/ip_source/iface_util.cpp`, on the loop thread | A kernel snapshot of local interface metadata: read-only system state, with a size the kernel fixes and no network round trip |
| startup file/loader I/O | `composition/assembly.cpp` and `composition/commands/`, on the main thread before `coro::run` | Config read, static validation, plugin `dlopen`, the environment check and the trust-context build happen once, before any coroutine exists |
| CA discovery + `SSL_CTX_load_verify_locations` | `infrastructure/network/tls/context.cpp`, off-loop only (`TlsContext::create`) | Reads the trust store once and shares one immutable context, so a handshake runs entirely from memory. Trust material is populated eagerly during `create()` through the `X509_STORE_load_path` / `X509_STORE_load_locations` family; this is a review invariant, confirmed by hand during review |
| plugin ABI cycle (`create`/`update`/`destroy`) | offload pool; cycles of one driver run concurrently on distinct instances, bounded by the pool's worker count | The C ABI is synchronous, so the cycle runs on a worker and waits until the bridge's HTTP child coroutine completes on the loop |
| log write | log drain thread (async spdlog sink) | The caller hands the record over and continues; a full queue discards the newest record |
| CPU-intensive work | any `coro::offload` call site | `offload` is the single exit for blocking *and* CPU-bound work (the `asyncio.to_thread` analogue); the total off-loop workload is structurally bounded by the configuration |

`src/support/util/` holds generic helpers only. A helper that needs to block — a
TTL cache with a single-flight mutex, a retrying sleep — routes its work through
`coro::offload`.

## Layers

Dependency direction is enforced by the CMake target graph
(`yaddnsc_domain` ← `yaddnsc_application` ← infrastructure/adapters ←
`yaddnsc_composition` ← executable) and policed textually by the
`architecture_guard` ctest (`cmake/ArchitectureGuard.cmake`).

- `src/domain/`: rules and value types. Values in, values out; `std::chrono`
  supplies time and duration values, and reading the system clock happens above.
  Domain types belong to `domain`, including DNS server settings, resolver strategy,
  IP source kind, record kinds, addresses and DNS errors. Raw JSON DTOs in
  `Config` reference these domain types; their Glaze mappings stay in the config adapter.
- `src/application/`: the coroutine use cases and the ports. It names domain
  types, the public coroutine APIs and the injected ports. Each external capability
  has its contract header under `ports/`, regardless of whether its operations are
  synchronous or coroutine-based; consumers include the contracts they use.
  Port headers contain contracts and their value types, not formatting or call-site
  conveniences. Logging uses the `ports/log.h` contract and the separate `log.h`
  formatting facade. spdlog, Glaze, CLI11, OpenSSL and the dynamic loader are
  reached from `src/infrastructure/`.
- `src/infrastructure/coro/`: the coroutine runtime — loop, `Task`, structured
  scopes, cancellation combinators, sleeps, `AsyncMutex`, `offload`, signals.
  Threads, futures and the thread pool live here, plus the plugin bridge's one
  blocking handoff.
- `src/infrastructure/network/`: lower-level network facilities grouped by
  concern. `address/` contains the socket-address codec, `transport/` the TCP/UDP
  streams, shared transport types and socket primitives, and `tls/` the TLS
  stream, trust helpers and OpenSSL diagnostics. `factory/` assembles TCP and
  TLS streams above their interface; `transport/` never includes `tls/`.
- `src/infrastructure/http/`: the coroutine HTTP client and protocol implementation,
  built on `network/` and shared by DNS-over-HTTPS and IP-source adapters.
  Hostname resolution is an injected `net::HostResolver`, not a concrete DNS
  dependency. Composition and the DoH factory bind explicit bootstrap servers;
  without a resolver, hostnames fail fast and IP literals still work.
- `src/infrastructure/uri/`: the lightweight URI codec shared by config,
  HTTP, DNS and CLI code; it depends on domain address parsing, not transport.
- `src/infrastructure/dns/`: the DNS wire layer plus the coroutine resolvers,
  the dispatcher, the factory and the resolver port adapter. `dns_classic` is
  the lower target.
- `src/infrastructure/ip_source/`: interface, HTTP and mDNS IP sources behind
  the IP-source port.
- `src/infrastructure/plugin/`: the plugin host — loader, catalog, instance
  lease, and the coroutine bridge and gateway. Loader/ABI failures use
  `plugin::PluginError` from `plugin_error.h`; business driver failures remain
  `domain::DriverError`.
- `src/infrastructure/config/`: JSON parsing, normalization, validation and
  parse diagnostics.
- `src/infrastructure/logging/`: the `LoggerPort` facade and the async sink.
- `src/cli/`: argument parsing and output presentation.
- `src/composition/`: the composition root. It assembles infrastructure and
  dispatches commands; driver-parameter validation and bounded DNS lookup live
  in application diagnostics and operate through ports.
- `src/support/`: internal shared helpers, forwarding to `include/yaddnsc/util/`
  where an equivalent public utility exists.
- `include/yaddnsc/sdk/` and `include/yaddnsc/util/`: the public surface — the
  plugin SDK, and the header-only utilities the host and plugins share.
  `include/yaddnsc/util/` is the single implementation site.
- `driver/`: bundled provider plugins, built against the public headers.

### Logging entry points

Each layer has exactly one designated diagnostic entry point. The requirement
behind this table is in [Quality & Process](../rules/04-quality-and-process.md#logging).
The Host Services adapter translates C ABI log records into the internal
`LoggerPort` contract; public SDK headers do not depend on that C++ port.
Changing the internal contract requires updating its host adapters, not the C ABI.

| Layer | Entry point |
|-------|-------------|
| Domain | Diagnostics belong to the caller; the layer holds no logging dependency |
| Application | the injected `app::LoggerPort` contract in `ports/log.h` and the `YLOG_*` formatting macros in `application/log.h` |
| Infrastructure / support | `SPDLOG_*` through the centrally configured backend; no independent sinks, and no application port introduced for logging alone |
| SDK / plugins | Host Services (`yaddnsc_host_services::log`); C++ helpers normally use the `YADDNSC_SDK_LOG_*` macros in `include/yaddnsc/sdk/driver.hpp` |
| CLI / composition | the same spdlog backend for host diagnostics; user-facing output is presentation and uses `std::print` / `std::println` on stdout/stderr |

## Plugin boundary (v1 alpha)

Drivers are runtime-loaded shared libraries talking to the host exclusively
through the **v1 alpha C ABI** (`include/yaddnsc/sdk/driver_abi.h`) plus an
optional C++ helper layer (`include/yaddnsc/sdk/driver.hpp`). What crosses the
`.so` boundary is C-representable: fixed-width integers, NUL-terminated
buffers, and plain structs with explicit sizes. Host Services carry the C++
services a plugin needs — HTTP, logging, cancellation.

- The host reads the 8-byte version prefix, then accepts the plugin when
  `yaddnsc_abi_provides` says the host provides the plugin's `abi_major` and
  `abi_minor`; the ABI 1.0 baseline `struct_size` is checked after the version.
  A mismatch rejects the plugin with a rebuild-with-current-SDK message.
- Four entry points are required (`get_descriptor`, `create`, `destroy`,
  `update`); a fifth, `yaddnsc_driver_validate`, is optional since ABI 1.0 — the
  host dlsym-probes it so `config test` can check `driver_params` against the
  driver's schema. Without it the plugin provides no such check, `config test`
  fails, and the plugin still loads and can update.
- Capability bits are enforced before the plugin runs: `A` and `AAAA` are
  delivered when the matching bit is set, and a record type outside that set
  ends as `DriverError::UPDATE_FAILED`, a host-side decision that produces no
  plugin ABI status.
- Each update runs on a fresh driver instance (`create → update → destroy`).
  Instances of the same module may update concurrently, and each instance
  serves one update at a time. Each instance runs its own `create()`,
  independent of process lifetime.
- Provider HTTP, logging and cancellation reach the plugin through Host
  Services: the host owns the HTTP client (reached through the bridge) and the
  log sink, forwarding source location to `spdlog::source_loc`.
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
