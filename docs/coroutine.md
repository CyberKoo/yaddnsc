# Coroutine runtime

The lifecycle contract and API boundary of the project's coroutine runtime —
what a change to the runtime or its consumers has to respect, stated as
contracts. The code is the record of the implementation. The thread and I/O
model this runtime implements is in [concurrency.md](concurrency.md); the
regression suites are in [testing.md](testing.md#coroutine-regression-tests).

## Coroutine lifecycle migration

(The section name is historical; this is the lifecycle contract.)

- **Ownership.** Only an unstarted task has RAII ownership. A started frame
  belongs to the runtime and is retired on the loop, in batch order, after
  the resume that finished it returned — never during final suspend or result
  consumption.
- **Publication.** A child's result becomes observable only after its frame
  has been destroyed. Unconsumed results — values and errors alike — stay
  owned by the scope and are released when it exits.
- **The body is an independent child**, retired before the scope drains its
  spawned children. Anything the children borrow must therefore outlive the
  body: owned by the scope runner, or by the caller outside the whole scope.
- **Drain.** Scope exit is the only full-drain boundary. Admission of new
  children closes when the body returns; an abnormal exit cancels the
  children; every child is joined before the outcome propagates; unobserved
  defects are reported, never dropped.
- **Results are single-consumer.** Claiming one reserves it; consumption
  commits when the wait resumes; cancelling the wait releases the reservation
  without consuming and wakes other observers. Observation by type reports
  exhaustion of that type only, and a released reservation makes its result
  available again.

### Coroutine lifecycle cost model

The lifecycle contract — frame retirement, ordered publication, result
ownership — dominates the cost of a started execution, not allocation. The
measured representation budgets live with the benchmarks; a layout change
needs a measured trade-off, not only a correctness argument, and must
preserve the ownership and phase rules above.

## Coroutine API boundary

- Application code names only the public surface: task and scope ownership,
  structured nurseries, cancellation combinators and their outcomes,
  cancellable sleeps, loop time as values, an explicit checkpoint, offload,
  and the mutex/notification primitives. The exact surface is enumerated by
  the architecture guard's allowlist; the list lives in the guard script, not
  here.
- Runtime internals — frames, awaiters, waiter and timer bookkeeping, result
  storage, loop scheduling state — are private to the runtime module. The
  guard rejects internal includes and symbols from every other module.
- A second, narrower host surface exists for code that bridges foreign
  threads into the runtime (the plugin host): a single-waiter notification
  primitive, a cancellation observer, and a nursery admission token. Host
  code uses these and nothing deeper.
- Loop and clock are composition and infrastructure interfaces, not
  application APIs; composition owns loop creation.
- Scopes are always borrowed — from the combinator that handed the body its
  scope, or implicitly from the current context. Reading a scope is not a
  checkpoint; cancelling one and re-observing cancellation are explicit
  calls.
- **Checkpoints.** The explicit checkpoint yields and observes cancellation;
  sleeps, descriptor waits, stop waits, mutex acquisition, offload, transport
  I/O and result waits are checkpoints too. Some thin wrappers observe
  cancellation without yielding; inline task composition adds no checkpoint.
- Header self-containment is build-enforced; include acyclicity inside the
  runtime is a rule stated in the headers themselves.
