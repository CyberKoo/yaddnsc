# Concurrency & I/O model

The host runtime's thread model, cancellation rules and offload mechanism —
the concurrency boundaries a change has to respect, stated as contracts. The
code is the record of their implementation. See
[architecture.md](architecture.md) for the layer overview,
[coroutine.md](coroutine.md) for the coroutine lifecycle and API contracts,
and [testing.md](testing.md) for the regression suites.

Axioms:

- the loop thread does loop work only — all I/O, timers and coroutine
  resumption happen there, plus bounded small computation;
- every coroutine belongs to a scope that joins its children;
- a deadline and a shutdown are the same thing: a cancelled scope;
- every resumption goes through the ready queue, so stack depth stays
  bounded.

**Threads.** Three roles: the loop thread; an offload pool, created lazily on
first use and sized from the hardware, which is the single exit from the loop
for blocking or CPU-bound work (plugin ABI cycles are its main user); and a
log drain thread. A command that never offloads never spawns a worker.

**Structured concurrency.** Tasks are lazy: awaiting one schedules the child
through the ready queue and resumes the parent only after the child has fully
retired — it is neither an inline call nor a cancellation checkpoint. Scopes
come in two supervision flavors: fail-fast cancels the siblings on the first
child defect, while supervisor keeps them running and reports defects for
observation. Either way, no child failure is silently lost: unobserved
defects are reported when the scope exits.

**Cancellation is scope state.** Deadline, timeout, cancel and shield
combinators each run a body in a child scope; cancellable waits are
checkpoints where a cancelled scope surfaces as a control-flow signal outside
the ordinary exception hierarchy. Because a deadline is composed at the call
site, lower layers take their time budget from the enclosing scope rather
than from timeout arguments. Cancellation is sticky and walks the scope tree
without allocating; broad catches must rethrow the control-flow signal
first. At the plugin boundary it is translated to and from the C ABI status
vocabulary, and only host-initiated cancellation is treated as control flow.
Cancellation hooks run synchronously and must not block, destroy scopes, or
re-enter the runtime.

**Leaving the loop.** Offloading runs a callable on the pool and delivers its
result through the loop. Cancelling the await abandons the work packet — the
await throws while the work finishes on its own — so a callable's captures
must outlive the worker; the joined form instead delays the unwind until the
worker actually finishes. Cooperative callables receive a stop request; a
non-cooperative one cannot be interrupted, and an indefinitely blocked worker
prevents shutdown from completing — there is no hard kill. Submitted work
drains before the runtime returns.

**The plugin bridge.** The plugin host maps ABI cycles onto these primitives:
a synchronous cycle occupies one worker for its whole duration; an
asynchronous cycle runs as loop-driven steps whose HTTP waits occupy no
worker. Every exchange with a plugin carries a wall-clock budget; cancelling
a cycle cancels its worker wait and its in-flight HTTP, while the resources
it borrowed survive to real completion.

## Blocking-operation inventory

Network waits never park the loop thread: sockets are non-blocking and TLS
waits on readiness. The remaining blocking or synchronous points are
sanctioned, and every new one needs the same scrutiny:

| Point | Where it lives | Bound |
|-------|----------------|-------|
| startup file and loader I/O | main thread, before the loop starts | once per process |
| trust material (CA bundle) | built off-loop into an immutable shared snapshot | rebuilt only explicitly; handshakes never touch the filesystem |
| network-interface metadata | offloaded live snapshot, no cache | the caller's deadline |
| entropy acquisition | synchronous, by documented exception | no wall-time bound; cryptographic failure is an operational error, never a silent fallback to weaker randomness |
| plugin ABI cycle | offloaded — a synchronous cycle holds one worker, an asynchronous step holds none | the per-exchange budget |
| log write | handed to the drain thread | a full queue discards the newest record |
| CPU-intensive work | any offload call site | structurally bounded by the configuration |

Anything not listed here must leave the loop through the offload primitive.
