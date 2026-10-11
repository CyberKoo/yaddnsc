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
main() -> Cli::parse -> Composition::run
  |                         (async logging install; config load, plugin dlopen,
  |                          /etc/resolv.conf, environment validation and
  |                          trust-context build on the main thread, before the
  |                          loop starts)
  v
coro::run(loop, app::run_root(config, services))
  |
  v
supervisor_group                root scope
  +-- signal watchers         SIGINT / SIGTERM -> cancel the work scope;
  |     |                     a second SIGINT drains logs and _exit(130)s
  +-- work scope              a second supervisor_group, a scope of its own,
        |                     cancelled by the watchers
        +-- subdomain_loop     one long-lived coroutine per subdomain; its
        |     |                pacing state (update interval, force-update
        |     |                latch, cycle start) lives in the frame
        |     +-- run_update_cycle    IP source -> DNS read -> decide -> driver update
```

The executable is a thin `main()` over `Cli::parse` and
`Composition::run`. The composition root is the only place concrete
infrastructure is assembled; the application layer reaches it through ports.
Each command handler owns its own presentation and exit code for its expected
failures; `Composition::run` owns the logging pipeline's lifetime around
dispatch and catches defects escaping it.

The work group is nested in the root group, and the watchers live in the outer
scope so the cancellation they trigger does not cancel them; the root cancels
them explicitly once the work has ended. If every loop ends without a
shutdown having been requested, the run root logs CRITICAL and returns
`EXIT_FAILURE`, so a supervisor with `Restart=on-failure` replaces the husk
instead of leaving it.

The diagnostic commands run on the same runtime with a loop of their own.
`dns resolve` bounds its lookup at 30s; `config test` validates every
subdomain's `driver_params` through the optional ABI entry in a one-shot
group whose scope owns the bridge coroutines.

The loop's signal watcher exists only while the loop runs, so a minimal
counting handler (`composition/startup_signals.h`) covers the gaps: it is
installed before config load, the blocking startup steps check the counters
between milestones, and the run root folds the counts into its shutdown
state — a SIGINT/SIGTERM caught during startup requests shutdown before any
work starts, and a repeat SIGINT still escalates. After the loop exits, the
drain tail checks the counters once more and answers a second SIGINT with
`_exit(128 + SIGINT)`.

## Concurrency & I/O model

The whole system is built on the coroutine runtime in
`src/coro/`. Its axioms: the loop thread does loop work only;
every coroutine belongs to a scope that joins its children; a deadline and a
shutdown are the same thing (a cancelled scope); every resumption goes through
the ready queue, so stack depth stays bounded.

**Threads.** There are at most three:

| Thread | Responsibility |
|--------|----------------|
| loop thread (`coro::run`'s caller) | all I/O, timers, coroutine resumption, bounded small computation |
| offload pool (`BS::thread_pool`, reached through `coro::offload`; min(hardware cores, 4) workers, at least 2) | the plugin ABI cycle and CPU-intensive work in general — `offload` is the runtime's `to_thread` analogue and the single exit from the loop |
| log drain thread (spdlog async sink) | writing log records off the loop |

The pool is created lazily by the first `coro::offload`, so it exists only
where something actually leaves the loop — the driver ABI cycles.
`Loop::set_offload_workers` overrides the default size before that first
offload. A command that never offloads never spawns a worker: the
presentation-only ones (`driver list`, `config show`, `interface list`,
`info`) run no loop at all, and the loop `dns resolve` builds for itself never
submits a job, so both run on two threads. `config test` builds a loop of its
own too, but it does offload — every subdomain's `driver_params` check is a
driver ABI cycle — and gets all three.

**Loop internals** (`src/coro/loop.h`): a `poll()` descriptor
table, a timer heap, a ready queue and a cross-thread inbox.

**Structured concurrency.** `coro::Task<T>` is lazy and runs inline at
`co_await`. `task_group` cancels siblings on the first failure and rethrows
after joining every child; `supervisor_group` reports a child failure through
`next()` while its siblings keep running. `spawn_discard` starts a
fire-and-forget child whose bookkeeping is shed at completion, which keeps a
process-lifetime group bounded by its live children. The run root uses a
supervisor group, so one subdomain failing leaves the others running.

The DNS dispatcher is the runtime's other large user. The concurrent strategy
races up to `MAX_CONCURRENT_RESOLVERS` (3) backends in a `task_group`: each
transient failure launches the next backend at once, the winner cancels its
losing siblings through a child scope, and scope exit still joins and reaps
every child. Retries wait on a linear cancellable backoff sleep on the loop,
not on a thread. The bootstrap resolver races its A and AAAA queries through a
`task_group` the same way.

Child results are single-consumer: `co_await Handle<T>` claims the result at
await entry, shared across handle copies and `next<T>()`. Joining an empty or
already claimed handle throws `std::logic_error`. `next<T>()` returns `nullopt`
as soon as there are no unclaimed children of type `T`, without waiting for
other result types. A Handle shares ownership of the child's bookkeeping slot,
so one that outlives the group is dead but memory-safe: `valid()` is false and
joining throws `std::logic_error`, the same failure as an empty or claimed
handle. The child frame itself is still destroyed at scope exit.
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
be created, or if another loop is already alive: the signal handler's
process-wide state serves exactly one loop per process, and a second live loop
would silently misroute the first loop's signals.

**Cancellation is scope state.** `with_timeout`, `with_deadline`,
`with_cancel_scope` and `non_cancellable` run a body in a child scope;
cancellable waits are checkpoints, and a cancelled scope makes them throw
`coro::Cancelled`, a control exception outside the `std::exception` hierarchy.
A timeout is composed at the call site (`co_await with_timeout(d, ...)`), which
is why the transport layer takes its deadline from the surrounding scope
rather than from an argument — its connect/read/write timeout options are gone
(`network/transport/options.h`). The HTTP layer is the exception:
`http::Options::connect_timeout` is a real per-address budget, 5s by default
and 1s for the DoH resolver, and an expired budget skips to the next resolved
address instead of failing the exchange. Cancellation is sticky until the
responsible scope exits; a scope absorbs its own cancellation while ancestor
cancellation propagates; groups join all children before propagating it, as
scope state rather than as a child defect. Cleanup runs inside
`non_cancellable`, then lets cancellation propagate. Broad coroutine catches
rethrow `Cancelled` ahead of other failures; plugin C ABI adapters convert it to
`YADDNSC_STATUS_CANCELLED`, and ABI values a plugin returns are converted back
at the driver gateway. A plugin that *returns* `YADDNSC_STATUS_CANCELLED`
while its host scope is still live is not treated as control flow — the
gateway logs it and converts it to a `DriverError::UPDATE_FAILED` value, so
the cycle is retried like any other update failure; only cancellation the
host actually initiated propagates as `Cancelled`.

### Coroutine API boundary

Application code names runtime capabilities and uses the values they return.
The implicit runtime context, loop and clock objects, coroutine frames,
registrations and scope waiter lists stay inside the runtime, which
implementations reach directly and share with each other. The application
surface is task ownership and structured groups (`task.hpp`, `group.hpp`,
`task_group.hpp`, `fwd.h`), the cancellation combinators (`scope.hpp`),
`ScopeOutcome` (`scope_outcome.hpp`), `CancelScope` and `coro::Cancelled`
(`cancel_scope.h`, `cancelled.h`), cancellable waits (`sleep.hpp`,
`signal.hpp`), loop time as values (`now.hpp`, `time.h`), `checkpoint()`
(`checkpoint.hpp`), `offload` (`offload.hpp`) and `MutexGuard`
(`async_mutex.hpp`, `mutex_guard.hpp`) — all declared in `src/coro/` and
enumerated by the guard allowlist in `cmake/ArchitectureGuard.cmake`. The
allowlist admits more than `src/application/` uses today: `offload` and
`current_scope()` are reached only by `plugin/driver_gateway.cpp`, and
`MutexGuard` only by `dns/resolver/doh.h`, `dns/resolver/dot.h` and
`http/session.h`.

Shared runtime implementation types — frames, promises, awaiters, waiter and
timer nodes, result storage, group bookkeeping, and loop/scope access helpers —
live in `src/coro/detail/` under `coro::detail`. Public classes
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
the shared `MutexState` are top-level types rather than nested members.
Acyclicity across `src/` is a rule stated in the headers themselves, not a
build check: what the build enforces is self-containment
(`cmake/HeaderCheck.cmake` compiles every first-party header as its own
translation unit).

`Loop` and `Clock` remain interfaces for composition, infrastructure and runtime
tests, not application APIs. Composition and infrastructure keep their startup,
I/O and runtime-internal access; runtime tests construct `Loop` and `ManualClock`
directly. Composition owns loop creation and the `coro::run` entry point. The
guard's coroutine header allowlist and internal-access checks apply only to
`src/application/`, not to composition, infrastructure or tests.

A scope an application holds is always the one a combinator passed to its body
or `TaskGroup::scope()`; `current_scope()` is the same borrow reached
implicitly, and only the driver gateway uses it
(`plugin/driver_gateway.cpp`). It is borrowed for the lifetime of that
combinator or group and used on the loop thread. Reading it is not a
checkpoint; cancelling it (`cancel()`) and re-observing cancellation
(`throw_if_cancelled()`) are explicit calls, and the latter is how the plugin
gateway turns a synchronous ABI abort into coroutine cancellation. The run
root borrows `work.scope()` and stores that borrow to cancel its work from a
sibling signal watcher, and clears it before the group scope is destroyed.

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
waits for it. Each exchange is bounded: the gateway's `bridge_wait_budget`
(5s) wraps the loop-side coroutine, and the worker waits 250ms longer, so the
loop's timeout always fires first and the worker observes a real error instead
of a bare wait timeout. Abandoning one update cancels that exchange's own
scope, so the plugin's HTTP stops at once.

**Subdomain loops.** Each subdomain is one long-lived coroutine that sleeps,
updates and repeats, reusing one `UpdateTask` across cycles and carrying only
its pacing state — update interval, force-update latch, cycle start — as frame
locals. There is no accumulated back-off state: each cycle recomputes its own
delay, a provider-supplied `retry_after` overriding the interval, anchored at
the failure when one is supplied and at the cycle start otherwise. Each cycle
is bounded by its own timeout, with a shorter one around the DNS read, so a
dead resolver surfaces in seconds. A subdomain loop exits under cancellation:
`coro::Cancelled` propagates out of the pacing sleep and the run root logs it
and rethrows, so the group's cancel ends the work phase. A defect escaping a
loop is logged and the guard restarts the loop after one update interval — the
legacy scheduler's queue also retried a failed workflow on the next interval,
so a defect never retires the subdomain, and a non-positive configured
interval is clamped to a minimum rather than spinning.

### Blocking-operation inventory

The loop thread stays non-blocking, and so is the socket path it drives:
sockets are opened with `O_NONBLOCK` (`network/transport/socket_ops.cpp`), so
connect, send, recv and the SSL calls return `EAGAIN` into an fd wait instead
of parking the thread — DNS, provider HTTP and the mDNS query (a non-blocking
datagram socket bounded by a 500ms `with_timeout` window) all run on the loop.
What is left blocks — a `getifaddrs()`, a file read, a `dlopen`, a provider
call — and each of those reaches the system through `coro::offload` or during
startup, before the loop starts. A few points sit on a specific thread by
design, each bounded:

| Blocking point | Where | Why it is bounded |
|----------------|-------|-------------------|
| `getifaddrs()` | `infrastructure/ip_source/iface_util.cpp`, on the loop thread | A kernel snapshot of local interface metadata: read-only system state, with a size the kernel fixes and no network round trip |
| startup file/loader I/O | `composition/assembly.cpp` and `composition/commands/`, on the main thread before `coro::run` | Config read, static validation, plugin `dlopen`, the environment check and the trust-context build happen once, before any coroutine exists |
| `/etc/resolv.conf` read | `infrastructure/dns/bootstrap/resolv_conf.cpp`, reached from `composition/assembly.cpp` on the main thread | A `std::ifstream` of a small system file, read once at startup to fill the bootstrap server list when none is configured |
| CA discovery + trust-store load | `infrastructure/network/tls/context.cpp`, off-loop only (`TlsContext::create`) | Bundle discovery is a function-local static, so the filesystem is touched once per process rather than per `create()`; trust material is then populated eagerly during `create()` through `SSL_CTX_load_verify_locations`, or `X509_STORE_load_path` on the OpenSSL default cert dir as the fallback — never the lazy registrars. One immutable context is shared, so a handshake runs entirely from memory |
| plugin ABI cycle (`create`/`update`/`destroy`) | offload pool; cycles of one driver run concurrently on distinct instances, bounded by the pool's worker count | The C ABI is synchronous, so the cycle runs on a worker and waits until the bridge's HTTP child coroutine completes on the loop |
| log write | log drain thread (async spdlog sink) | The caller hands the record over and continues; a full queue discards the newest record |
| CPU-intensive work | any `coro::offload` call site | `offload` is the single exit for blocking *and* CPU-bound work (the `asyncio.to_thread` analogue); the total off-loop workload is structurally bounded by the configuration |

`src/support/util/` holds generic, non-blocking helpers only — algorithm
search, byte order, file-descriptor ownership, randomness (which seeds from
OpenSSL's `RAND`) and input validation. None of them blocks; a helper that
needs to will route its work through `coro::offload` rather than park its
caller.

## Layers

Dependency direction is enforced by the CMake target graph
(`yaddnsc_domain` ← `yaddnsc_application` ← infrastructure/adapters ←
`yaddnsc_composition` ← executable) and policed textually by the
`architecture_guard` ctest (`cmake/ArchitectureGuard.cmake`). That sketch
coarsens a graph of about twenty module targets — one per concern
(`yaddnsc_coro`, `yaddnsc_uri`, `yaddnsc_net`, `yaddnsc_http`,
`yaddnsc_dns_classic`, `yaddnsc_dns_bootstrap`, `yaddnsc_coro_dns`, the
`yaddnsc_*_infrastructure` and `yaddnsc_*_adapter` sets). `src/application/`
is split in two: `yaddnsc_application` carries the diagnostics and the
environment validator, `yaddnsc_coro_application` the run root, the subdomain
loop and the update cycle. `yaddnsc_coro` is a base module beside
`yaddnsc_domain` — it depends on neither, and the application names only its
public headers.

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
  conveniences. Logging uses the `ports/log.h` contract and the separate
  `application/log_macros.h` formatting facade. spdlog, Glaze and the dynamic
  loader are reached only from `src/infrastructure/`; CLI11 from
  `src/cli/parser.cpp`, and OpenSSL also from `src/support/util/random.hpp`.
- `src/coro/`: the coroutine runtime — loop, `Task`, structured
  scopes, cancellation combinators, sleeps, `AsyncMutex`, `offload`, signals.
  The thread pool lives here, owned by `Loop` and sized by
  `default_offload_workers()`, but the runtime keeps no futures of its own:
  `offload` carries a result through the loop's single cross-thread channel
  rather than a `std::future` — the tree's only `std::promise` belongs to the
  plugin bridge's blocking handoff. Loop trace diagnostics leave through the
  injected `Loop::TraceSink`; the runtime itself holds no logging backend.
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
  `Uri::resolve()` owns authority-based reference resolution and dot-segment
  removal (RFC 3986 §5.2); `parse()` retains its lenient bare-host semantics.
  Resolution preserves repeated slashes, percent escapes and explicit empty
  queries, discards fragments, and requires a base with scheme and host.
  HTTP owns redirect status, protocol/downgrade and credential policies.
- `src/infrastructure/dns/`: the DNS wire layer plus the coroutine resolvers,
  the dispatcher, the factory and the resolver port adapter. `dns_classic` is
  the lower target. The factory maps the address URI scheme to a closed set of
  backends (classic, DoT, DoH); there is no runtime registry — tests inject
  fakes through the `Dispatcher` constructor and the resolver options' factory
  and resolve seams.
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
  plugin SDK, and the header-only utilities whose single implementation site
  is `include/yaddnsc/util/`. The host includes them directly; the guard bans
  `yaddnsc/util/*` from `driver/**`, so a plugin reaches the same utilities
  through the SDK re-exports (`include/yaddnsc/sdk/format.hpp`).
- `driver/`: bundled provider plugins, built against the public headers.
- `plugin_support/crypto/`: crypto helpers bundled for the plugins that sign
  requests (`signing.cpp/h`, target `yaddnsc_plugin_support_crypto`, installed
  to `include/yaddnsc/plugin_support/crypto`). Linked by `alibaba_cloud` and
  `route53`; the host executable never links it.

### Logging entry points

Each layer has exactly one designated diagnostic entry point. The requirement
behind this table is in [Quality & Process](../rules/04-quality-and-process.md#logging).
The Host Services adapter translates C ABI log records into the internal
`LoggerPort` contract; public SDK headers do not depend on that C++ port.
Changing the internal contract requires updating its host adapters, not the C ABI.

| Layer | Entry point |
|-------|-------------|
| Domain | Diagnostics belong to the caller; the layer holds no logging dependency |
| Coroutine runtime | the injected `Loop::TraceSink` (`coro/loop.h`), wired to the central backend by composition — only in trace-enabled builds, where `#if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE` compiles the sink in and leaves it null otherwise; the module itself holds no logging dependency |
| Application | the injected `app::LoggerPort` contract in `ports/log.h` and the `YLOG_*` formatting macros in `application/log_macros.h` |
| Infrastructure / support | `SPDLOG_*` through the centrally configured backend; no independent sinks, and no application port introduced for logging alone |
| SDK / plugins | Host Services (`yaddnsc_host_services::log`); C++ helpers normally use the `YADDNSC_SDK_LOG_*` macros in `include/yaddnsc/sdk/driver.hpp` |
| CLI / composition | the same spdlog backend for host diagnostics; user-facing output is presentation and uses `std::print` / `std::println` on stdout/stderr |

## Plugin boundary (v1)

Drivers are runtime-loaded shared libraries talking to the host exclusively
through the **v1 C ABI** (`include/yaddnsc/sdk/driver_abi.h`) plus an
optional C++ helper layer (`include/yaddnsc/sdk/driver.hpp`). What crosses the
`.so` boundary is C-representable: fixed-width integers, NUL-terminated
buffers, and plain structs with explicit sizes. Host Services carry the C++
services a plugin needs — HTTP, logging, cancellation.

- The host reads the 8-byte version prefix, then accepts the plugin when
  `yaddnsc_abi_provides` says the host provides the plugin's `abi_major` and
  `abi_minor`; the ABI 1.0 baseline `struct_size` is checked after the version.
  A rejection names its remedy: a major the host does not implement takes
  `use a driver built for ABI v1.`, as do a too-small `struct_size` and
  missing entry points, while a plugin needing a newer minor takes
  `upgrade yaddnsc.`.
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
