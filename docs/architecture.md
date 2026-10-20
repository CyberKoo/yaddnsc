# Architecture Notes

This is a maintainer-oriented overview. It records the boundaries a change has
to respect, at the level of layers and mechanisms; the code itself remains the
record of how any one module is built. The document deliberately describes
coupling, not implementation: symbol names, module inventories and other
details that move with refactors are intentionally omitted — read the code for
those. Companion documents: [runtime-flow.md](runtime-flow.md) for the
business update cycle, [concurrency.md](concurrency.md) for the thread and
I/O model, [coroutine.md](coroutine.md) for the coroutine runtime contracts,
[plugin-abi.md](plugin-abi.md) for the driver ABI contract, and
[custom-drivers.md](custom-drivers.md) for the driver guide.

## Layers

The dependency direction is

`domain` ← `application` ← `infrastructure` ← `composition` ← executable

with the coroutine runtime and the domain as independent bases that depend on
nothing above them. The direction is enforced by the CMake target graph and
the `architecture_guard` ctest; the guard's concrete check list lives with
its script and is not restated here.

- `src/domain/`: business rules and value types. Values in, values out; no
  infrastructure, no third-party libraries, no clock reading.
- `src/application/`: the use cases, and the ports through which they reach
  external capabilities. Each capability has a contract header under `ports/`;
  port headers carry contracts and value types, not formatting or call-site
  conveniences. Application code names domain types, the public coroutine API
  and the injected ports — concrete implementations, third-party libraries
  and the dynamic loader are reached only from the layers below.
- `src/coro/`: the coroutine runtime, a base module beside the domain. See
  [concurrency.md](concurrency.md) and [coroutine.md](coroutine.md).
- `src/infrastructure/`: the implementations of external capabilities —
  network, HTTP, DNS, IP sources, the plugin host, configuration, logging —
  each behind the port it serves. A lower module offers its capability upward
  through injection and never includes a concrete sibling; adapters bind
  ports to implementations.
- `src/cli/`: argument parsing and output presentation. User-facing output is
  presentation, not logging.
- `src/composition/`: the composition root — the only place concrete
  infrastructure is assembled. Shared assembly recipes live here, so no
  command handler re-derives them.
- `src/support/` and `include/yaddnsc/`: shared utilities and the public
  surface (the plugin SDK). Utilities have a single implementation site;
  plugins reach them only through SDK re-exports.
- `driver/`: bundled provider plugins, built against the public SDK only.

### Logging entry points

Each layer has exactly one designated diagnostic entry point. The requirement
behind this table is in [Quality & Process](../rules/04-quality-and-process.md#logging).

| Layer | Entry point |
|-------|-------------|
| Domain | none — diagnostics belong to the caller; the layer holds no logging dependency |
| Coroutine runtime | an injected trace sink, wired to the central backend by composition; the module itself holds no logging dependency |
| Application | an injected log port, plus its formatting facade |
| Infrastructure / support | the centrally configured backend; no independent sinks |
| SDK / plugins | the Host Services log callback; C++ helpers use the SDK log macros |
| CLI / composition | the same backend for host diagnostics; user-facing output is presentation |

The Host Services adapter translates C ABI log records into the internal
contract, so changing the internal logging contract touches the host
adapters, never the C ABI.

## Manual boundary review

The architecture guard enforces the boundaries above textually — includes,
layering, thread facilities, and the rule that `rules/` stays fact-free. What
it cannot judge is reviewed by hand, on every change that touches the
corresponding area:

- **No blocking work on the loop thread**, including lazy loaders hidden in
  system or third-party libraries (trust stores, entropy sources). Blocking
  or CPU-bound work leaves the loop through the offload primitive — see the
  [blocking-operation inventory](concurrency.md#blocking-operation-inventory).
- **Clock domains do not cross as absolute timestamps.** Only remaining
  durations are translated between a worker clock and the loop clock.
- **Simplifications preserve cancellation.** Empty buffers, invalid streams
  and other fast paths must still observe an already-cancelled scope.
- **Textual checks have limits.** Macros, generated code, templates, forward
  declarations and other non-ordinary constructs can evade the guard; review
  those forms by hand rather than trusting the check.

## Plugin boundary (v1)

Drivers are runtime-loaded shared libraries talking to the host exclusively
through the **v1 C ABI** plus an optional C++ helper layer. What crosses the
`.so` boundary is C-representable: fixed-width integers, length-delimited
borrowed buffers, and plain structs with explicit sizes. The ABI contract —
versioning, verification, struct layout, validation, capability, cancellation
and lifetime rules — lives in [plugin-abi.md](plugin-abi.md); the
driver-authoring guide is [custom-drivers.md](custom-drivers.md).

Host-side wiring:

- The loader fixes one execution group per module. A synchronous cycle runs
  on an offload worker; an asynchronous cycle is driven as loop-side steps
  whose HTTP waits occupy no worker.
- Host Services contexts track a cycle's phase and are destroyed with it;
  HTTP is accepted only during an update.
- The host owns the HTTP client and the log sink behind Host Services, and
  calls every plugin entry point through an exception firewall.
- Each update runs on a fresh driver instance; instances of one module may
  update concurrently, each instance serving one update at a time.

See [plugin-abi.md](plugin-abi.md) before changing the interface, and
[custom-drivers.md](custom-drivers.md) before building a module outside the
project build.
