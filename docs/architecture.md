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
coro::run(loop, app::main_loop(config, services))
  |
  v
with_nursery                    fail-fast root scope; owns gateway/services
  +-- stop watcher            await the process adapter's latched stop request
  +-- work nursery            SUPERVISOR, cancelled by the stop watcher
        +-- startup validation   every record's driver_params, under a per-record budget
        +-- subdomain_task     one long-lived coroutine per subdomain; its
        |     |                pacing state (update interval, force-update
        |     |                completion time, cycle start) lives in the frame
        |     +-- update_task         IP source -> DNS read -> decide -> driver update
```

The executable is a thin `main()` over `Cli::parse` and
`Composition::run`. The composition root is the only place concrete
infrastructure is assembled; the application layer reaches it through ports.
Each command handler owns its own presentation and exit code for its expected
failures; `Composition::run` owns the logging pipeline's lifetime around
command execution and catches defects escaping it. The same run entry point
is used by the executable and command component tests.

The work nursery isolates subdomain failures. Root-child defects cancel the run;
runner-owned Resources keep gateway and service borrows alive through drain.
If every update task ends without a stop request, the main loop logs CRITICAL
and returns `EXIT_FAILURE`.

**Stop signals.** Composition owns one runtime `coro::StopSignals` instance before startup
and keeps it alive through `coro::run`, plugin unload and log drain. The same
SIGINT/SIGTERM handler stays installed for that whole lifetime. A first stop
request writes one byte to a nonblocking pipe; the byte is not consumed, so
requests before or during a coroutine wait remain observable. The application
queries the latched request before starting work and awaits it through injected
callbacks. The adapter waits with the runtime's cancellable `wait_readable`,
without periodic latch polling or busy-waiting; only if the loop's event source
itself is impaired (a sustained poll() failure fails the wait) does the wait
fall back to observing the atomic stop latch on the timer heap, which does not
depend on poll(). Synchronous startup deliberately
checks the latch after configuration load, plugin loading and trust-context
creation, then before validation and update-task launch. These milestone checks
let each blocking step finish before a graceful stop; exit-code selection also
queries the same latch. There is no handler handover, count merge or queue-round
contract. `StopSignals` is the sole process-signal facility; Loop owns no signal
handlers.
A second SIGINT calls `_exit(128 + SIGINT)` directly from the handler, including
while a plugin worker or log drain is blocked. Destruction restores the previous
handlers after workers and logging have finished. The runtime owns the raw process signal state; application code sees only the two stop callbacks.

**Startup validation.** Each record's `driver_params` are checked through the
optional ABI entry before any update task is spawned, inside the cancellable
work scope and under a per-record budget (5s, injected by the composition root
as `CONFIG_VALIDATION_BUDGET`) so each record has a bounded validation wait and a
shutdown signal still aborts the wait. Records sharing an identical
(driver, `driver_params`) pair are checked once, so duplicated records do not
multiply the wait. A synchronous plugin call already
running on a worker must still finish before `coro::run` returns. A record whose
driver explicitly rejects its parameters, or whose budget expires, fails the startup with `EXIT_FAILURE`
instead of publishing updates against a configuration the driver says is
wrong. A driver that exports no validation entry is warned about and skipped
under `run`, because refusing to start would break a configuration that updates
correctly; `config test` requires the entry, since answering "is this
configuration good?" is that command's entire purpose. Record types the driver
has no ABI capability for are rejected earlier still, by the environment
validator, so a mismatched `A`/`AAAA` is a configuration error rather than a
per-cycle failure.

The diagnostic commands run on the same runtime with a loop of their own.
`dns resolve` bounds its lookup at 30s; `config test` validates every
subdomain's `driver_params` through the optional ABI entry in a one-shot
supervisor nursery whose Resources own the gateway through bridge retirement.

Normal shutdown drains submitted workers and logs. Forced termination may lose
queued diagnostics; it never waits for that drain.

## Concurrency & I/O model

The whole system is built on the coroutine runtime in
`src/coro/`. Its axioms: the loop thread does loop work only;
every coroutine belongs to a scope that joins its children; a deadline and a
shutdown are the same thing (a cancelled scope); every resumption goes through
the ready queue, so stack depth stays bounded.

**Threads.** There are three roles (the worker role may contain multiple threads):

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

**Structured concurrency.** `coro::Task<T>` is lazy. An ordinary `co_await`
schedules the child through the ready queue, then resumes the parent only after
its complete retirement; it is not an inline call or a cancellation checkpoint.
`with_nursery` owns its body factory and optional Resources at stable addresses.
`NurseryPolicy::FAIL_FAST` cancels siblings on the first child defect and rethrows
after draining; `SUPERVISOR` keeps siblings running and reports observed defects
through `next()`/Handle. At exit every unobserved child defect, including discard
results, is reported in `GroupError::errors()`; supervision cannot lose failures.
All started frames belong to execution records and are destroyed by the loop's
FIFO batch-end retirement transaction, never during final suspend or result
consumption. Results become READY only after frame destruction. Unconsumed
results and errors, not completed frames, remain until nursery exit. The main
loop guards subdomain failures so siblings keep running.

The DNS dispatcher is the runtime's other large user. The concurrent strategy
races up to `dns::Policy::max_concurrent` (3 by default) backends in a fail-fast nursery: each
endpoint failure or negative answer launches the next backend at once, the winner cancels its
losing siblings through a child scope, and scope exit still joins and reaps
every child. Retries wait on a linear cancellable backoff sleep on the loop,
not on a thread. INVALID_QUERY alone ends a group search; PARSE/CONFIG are
endpoint diagnoses. Sequential and concurrent strategies share error precedence:
INVALID_QUERY, CONFIG, PARSE, operational failure, NODATA, NXDOMAIN. Dispatcher
input preflight builds an unsent fixed-ID packet and does not acquire entropy.
Only actual backend/bootstrap queries acquire secure IDs; their entropy failures
return CONNECTION, so dispatcher failover/retry remains reachable rather than
losing the error to an exception from preflight. Invalid names still return
INVALID_QUERY before a backend is called, even when the entropy source is down.
A failed view must not be turned into a definitive negative answer. Bootstrap races a bounded
set of servers per family, replacing failures immediately; silent servers do
not delay the other candidates in that set. It does not retry a server in one
lookup, and shares the classic wire exchange with Dispatcher backends. It
starts A/AAAA children in a fail-fast nursery; after the first success, a 50ms
`with_timeout` bounds the next result wait without consuming a late result.
The enclosing collection scope then cancels/joins remaining children while
preserving collected addresses; there is no synthetic timer result child.

DNS response matching compares opcode, ID and semantic QNAME/QTYPE/QCLASS,
independent of case or compression encoding, while preserving label boundaries.
The parser filters IN answers by owner and the in-answer CNAME chain; bootstrap
also enforces A/AAAA lengths. Fast and full parser entry points share EDNS option
validation and preserve the full 12-bit RCODE. CNAME-only replies without SOA
negative evidence and referrals are unusable endpoint answers, not NODATA;
there is no recursive referral chasing. mDNS has its own QR/opcode/RCODE/CLASS
checks, masking the cache-flush bit. TCP/DoT/DoH accept at most 16KiB as a resource
policy, not a DNS protocol limit. DoH enforces HTTPS and this cap even for direct
construction, uses HTTP/1.1 and rejects all redirects as a local safety policy.

CNAME traversal accepts at most 16 alias hops, bounding repeated answer scans
and cycle checks even on hostile packets. Shared DNS wire/exchange helpers must
not include the higher-level resolver facades; architecture_guard checks this.

The DDNS decision remains fail-open on lookup errors: it still attempts the
provider update. SKIP_UNCHANGED requires a nonempty RRset whose addresses all
match the local IP. DNS cannot identify the provider's configured record_id, so
one matching sibling must not hide a stale managed record. Mixed RRsets may
therefore cause a provider request every cycle; the driver still updates only
its configured record or record set.

Child results are single-consumer: `co_await Handle<T>` reserves the result at
await entry, shared across handle copies and `next<T>()`, and commits consumption
only when the wait resumes successfully. Cancelling releases the reservation. Joining an empty or
already claimed handle throws `std::logic_error`. `next<T>()` returns `nullopt`
as soon as there are no unclaimed children of type `T`, without waiting for
other result types. This is not permanent exhaustion: later spawns or released
reservations may make results available again. Pre-resume reservations do not
change publication order: reserved entries retain their completed-queue position
and are skipped until consumed or released.
Handle claims notify parked `next()` callers immediately,
even before the claimed producer finishes. A Handle shares ownership of the child's bookkeeping slot,
so one that outlives the nursery is dead but memory-safe: `valid()` is false and
awaiting throws `std::logic_error`, as for an empty or claimed handle. Handle and
next consumption require the owning loop and a structured descendant of the
nursery runner, including READY fast paths. Consumption removes the result
entry, never destroys a frame. Retained invalid handles keep empty storage only.
Moving a result can throw, but consumption is already committed and cannot retry.

`next<T>()` observes its caller's current cancel scope, not the group's scope.
A cancelled waiter detaches its subscription and releases any pre-resume result
reservation without consuming the result or cancelling the producers. Completion/cancellation enqueue at most one resume;
sticky cancellation wins before result consumption. Handle result waits follow
the same caller-scope rule, without cancelling the producer. Nursery exit is the
only full drain boundary: use a nested or sequential `with_nursery` when work
must continue after all children finish. There is no public join/with_children.

The body is an independent inline child, retired before the runner drains the
nursery children. Neither body locals nor body parameters remain available for
children after body return. Put borrowed owners in runner Resources or outside
the entire nursery await; pointer/reference members still require backing owners.
The runner closes admission on body return (or fail-fast), cancels on abnormal
exit, and drains without a cancellation subscription or new framework allocation.
After drain it may allocate to aggregate errors, then invalidates every result,
clears payloads, destroys Fn, Resources and nursery state in that order. Aggregate
OOM takes the same no-throw cleanup path before propagating bad_alloc.

Only unstarted frames have Task/awaiter RAII ownership; started frame retirement
is independent of result moves, including throwing moves. Slot and wait registrations commit only after their fallible
allocations succeed. StopSignals reports pipe/handler installation failures
before command execution and rolls back partial handler installation. A latched
stop wait still checks caller cancellation. Awaiting an empty or previously
awaited Task throws `std::logic_error`; closed nonnegative descriptors report
`POLLNVAL` as an invalid fd, not successful readiness.
`coro::run` requires a fresh loop and structured root completion. Task defects
still propagate normally. Exceptions escaping loop dispatch call
`std::terminate`: losing a dispatch batch cannot be recovered by safely
unwinding parked frames. External stop before root completion is likewise
fatal. Loop construction throws if its wake pipe cannot be created. Loop
instances own independent scheduling, inbox and wake-pipe state; only the
process-wide StopSignals owner is exclusive. Wake-pipe writes retry `EINTR`
and tolerate `EAGAIN` because a full pipe is already readable; other wake
failures are fatal. Poll failures are classified: interruptions return to
inbox/timer processing; `ENOMEM` is an operational event-source failure — the
loop paces itself on a capped backoff, and once the failure is sustained it
fails every registered fd wait with the poll errno (fd waits resolve to an
`std::error_code` instead of readiness) and keeps running, so in-flight
queries shed load rather than die with the process. `EINVAL`/`EFAULT` indict
the loop's own call construction and stay on the fatal dispatch path.
Fd registration reserves both poll-array and callback-snapshot storage before
linking the wait; allocation failure leaves it unlinked. Building either
dispatch snapshot therefore needs no buffer growth. Dispatch copies each
callback by index, so registering more descriptors in a callback may grow the
buffers without invalidating the current traversal. Unregistering a descriptor
also revokes its pending snapshot callback before its context may be destroyed;
this applies to both readiness dispatch and sustained poll-failure shedding.
Distinct registrations stay independent even with identical callbacks and contexts.
Optional trace formatting
remains best-effort and contains its own allocation failures.

Group bodies cancel their children on every abnormal exit, including cancellation
propagated from a nested deadline scope whose manual cancellation did not cancel
the group itself. They still drain all children before propagating the outcome.
Relative sleep/timeout deadlines and internal time differences saturate at their
representation limits. Nonpositive sleeps yield without waiting; nonpositive
timeouts arm already-due deadlines. Scope results require move construction,
not move assignment.

Cancel scopes borrow intrusive parent/child/sibling links: registration and
removal allocate nothing, and children retain creation order. Cancellation
walks the unshielded subtree iteratively in depth-first order, using constant
auxiliary space and native stack regardless of scope depth. Waiter order and
shield boundaries are unchanged; cancelling a shield itself still reaches its
unshielded descendants. A scope must outlive its children and parked waiters;
destruction with either still present is assertion-diagnosed and fatal in
production. Cancellation hooks, including worker stop callbacks, may run
synchronously and must not block, destroy scopes or re-enter the runtime.

**Cancellation is scope state.** `with_timeout`, `with_deadline`,
`with_cancel_scope` and `non_cancellable` run a body in a child scope;
cancellable waits are checkpoints, and a cancelled scope makes them throw
`coro::Cancelled`, a control exception outside the `std::exception` hierarchy.
A timeout is composed at the call site (`co_await with_timeout(d, ...)`), which
is why the transport layer takes its deadline from the surrounding scope
rather than from an argument — its connect/read/write timeout options are gone
(`network/transport/options.h`). The HTTP layer is the exception:
`http::Options::connect_timeout` is a real per-address budget, 5s by default
and at most 500ms for the DoH resolver; an expired budget skips to the next
resolved address instead of immediately failing the exchange. HTTP sessions may
also bound local queueing with `queue_timeout` (disabled by default); a queued
timeout never closes the active owner's connection. DoH/DoT wait for their
single session under the caller's scope, before starting an attempt. Admission
does not become an endpoint error or consume a Dispatcher retry. The admitted
attempt defaults to 1.5s for bootstrap/connect/I/O, with a 500ms per-address
connect cap. Session ownership stays exclusive throughout an exchange; a
cancelled waiter leaves the active connection untouched. DNS policies are
assembled in `composition/assembly.h` and copied into the factory, dispatcher,
backends and bootstrap resolver. Application stage budgets travel in
`app::UpdatePolicy`, injected through the services bundle.
Classic TCP connect/send/read
share a 1s wire-exchange budget instead of independent clocks, subject to the
remaining 1.5s attempt budget. A late truncated UDP answer may leave less than
1s for TCP; timeout diagnostics retain the TCP-fallback stage and RETRY code.
UDP validates local questions before I/O, discards unrelated replies, and reports
malformed matching replies as PARSE. Discard bursts periodically back off (1ms per 32 discarded datagrams) without
resetting the deadline or letting foreign packets force an early failure. mDNS
uses the same burst limit within its 2s response window.
Only Dispatcher retries entire DNS queries. A single backend defaults to two
attempts; multiple backends exhaust alternatives first, then perform at most one
recovery round for CONNECTION/RETRY failures (disabled when max_retries is zero).
Negative answers, malformed responses and unknown RCODEs are not retried
against the same backend, regardless of backend count. Unexpected exceptions
propagate to the operation/supervision boundary rather than becoming UNKNOWN
DNS failures; known parser exceptions are translated at one-attempt boundaries.
A recovery result supersedes that backend's earlier
failure when aggregating the final diagnosis. Two default attempts
plus 50ms backoff take 3.05s of admitted work for a single backend or one
concurrent wave. Admission and longer candidate lists remain subject to the
application's 4s caller bound; stage budgets do not guarantee every candidate
will complete.
Direct backend calls perform one attempt. Failed group results preserve counts
of negative responses alongside the operational diagnosis, without treating an
unanswered view as NXDOMAIN. Positive answers still take precedence; there is no
new early-negative timer. A lost
reused DoH/DoT connection may be repaired once within that same attempt's
remaining budget, not with a restarted clock. DoH marks its read-only POST as
explicitly replay-safe; ordinary HTTP POST requests are not automatically replayed. Cancellation is sticky until the
responsible scope exits. The first local cancellation trigger retains its cause,
including during
shielded cleanup. Deadline combinators absorb only their own timeout:
a non-timeout return always contains the body result, and manual cancellation
(including body-initiated cancellation) propagates. Explicit cancel scopes
absorb their own cancellation; groups join all children before propagating it, as
scope state rather than as a child defect. Cleanup runs inside
`non_cancellable`, then lets cancellation propagate. Broad coroutine catches
rethrow `Cancelled` ahead of other failures; plugin C ABI adapters convert it to
`YADDNSC_STATUS_CANCELLED`, and ABI values a plugin returns are converted back
at the driver gateway. A plugin that *returns* `YADDNSC_STATUS_CANCELLED`
while its host scope is still live is not treated as control flow — the
gateway logs it and converts it to a `DriverError::UPDATE_FAILED` value, so
the cycle is retried like any other update failure; only cancellation the
host actually initiated propagates as `Cancelled`. Validation follows the same
distinction: a plugin-reported CANCELLED on a live scope becomes a validation
error value, so config test reports failure instead of silently absorbing a
newly manufactured host cancellation.

### Coroutine lifecycle migration

The internal C++ coroutine API uses one structured nursery boundary. It does not
change the plugin C ABI.

| Former API | Replacement |
|------------|-------------|
| `task_group(fn)` / `TaskGroup&` | `with_nursery(fn)` / `Nursery&` |
| `supervisor_group(fn)` | `with_nursery(fn, NurseryPolicy::SUPERVISOR)` |
| `join()` / `with_children()` | complete a nested nursery await before continuing |
| `GroupToken` / `group_token()` | host-only `NurseryToken` / `nursery_token()` |

Move resources borrowed past body return into the nursery runner. For example,
assuming `send_report` returns `Task<void>` and borrows its string argument:

```cpp
co_await coro::with_nursery(
    std::string{"report"},
    [](coro::Nursery& nursery, std::string& report) -> coro::Task<void> {
        nursery.spawn(send_report(report));
        co_return;
    });
// send_report's frame has retired before the runner releases report.
```

Both the resource and factory inputs are taken by value and support move-only
owners. The factory is invoked as an lvalue at its stable address. Moving a
reference, pointer or view into Resources does not extend its backing owner's
lifetime. A body-local coroutine lambda also needs its own closure owner until
its task retires; nursery ownership does not preserve arbitrary body locals.

Consume Handle/next results only on the owning loop within the runner's
structured descendants. Cancellation releases a result reservation, not the
producer; a value move that throws still consumes the result. Full drain is
represented by leaving a nursery, not by consuming the last matching result.

All started frames retire through the loop after their resume returns. FIFO
retirement publishes results only after frame destruction and performs inline
parent notification in the same batch-end transaction. Ordinary inline chains
therefore gain no empty retirement batches, but same-batch Handle/next fast
paths disappear for terminated producers that have not retired yet. Result
invalidation clears payloads before factory, Resources and nursery state are
released, including aggregation failure. Host ingress tokens close before drain,
so they must be replaced, never reused, across successive nursery phases.

#### Coroutine lifecycle cost model

The lifecycle contract, not allocation, dominates the cost of a started
execution. Compared with the former model, one awaited level pays for:

- destroying the producer frame in the loop's retirement transaction, after the
  resume that reached final suspend returned;
- publishing the result only after that destruction;
- resuming the consumer only after publication;
- a record outside the frame that holds the frame, its context and the result.

The measured gap against a model that reads a value from a live child frame and
destroys it in the consumer's resume is therefore structural. Removing Release
invariants or the retirement barrier is not an acceptable way to recover it.
Earlier profiles identified retirement, queue manipulation and result/wait
handling as costs; their ranking must be remeasured after representation changes.
The current layout budgets are 120 bytes for an execution record, 168 bytes for
an int inline awaiter and 208 bytes for an int child entry. These are measured
implementation budgets, not required representations or theoretical minima.
A layout change needs a measured trade-off, not only a correctness argument;
it must preserve independent memberships and all lifetime and phase checks.
Fixed completion callbacks share immutable role descriptors; each record keeps
its own context, without dynamic registration or an extra allocation. The ready
hook stores a borrowed frame handle bound at adoption, avoiding a record lookup
before resume; the reap hook still targets the owning record. Both hooks retain
independent membership and duplicate-insertion checks. The ready token confers
no destruction permission and is valid throughout its queue membership; only
the record/reaper owns and destroys a started frame. An unlinked hook may retain
a stale token, which is never dereferenced by its destructor. This representation
adds no storage or allocation. The empty
inbox check is inline, while nonempty FIFO dispatch stays out of line; it retains
the relaxed peek followed by acquire exchange, and a concurrent post still wakes
poll for the next turn.

A ready batch is a scheduling quantum. Frames scheduled while a batch runs go to
the next batch, and a frame destructor must not dispatch the inbox or start a
child inline. The whole detached batch is resumed and reaped before the drain
returns; fusing retirement into the per-frame resume would recover throughput
but changes destructor ordering, and it is not part of this contract. A large
ready burst can therefore delay `poll()` by its length; bounding the batch
would move retirement into the middle of a batch, so it remains an open design
question rather than an implemented policy. No bounded timer/I/O tail-latency
guarantee follows from the poll budget: ready-burst size and frame-destructor
cost must be measured separately before choosing a service-level budget.
The separate coroutine observation benchmark makes a timer due and a pipe
readable inside a ready burst, recording real-clock execution, parameter-
destruction and service spans. Its controlled timer eligibility is not a
production-clock deadline or external I/O arrival measurement. It informs this
open design question without changing admission, batch formation or retirement.

Unconsumed payloads remain owned by the nursery result slots after producer
frames retire; retained bytes therefore scale with outstanding results and
payload size, not with the age of the retired frames. Consumption or nursery
exit releases payloads even if invalid Handle copies survive. The observation
benchmark measures this ownership plateau separately from producer-frame
lifetime; it does not measure RSS or make retained entry bookkeeping disappear.
Neither a payload retention limit nor a lighter invalid-Handle representation
is justified without a target budget and a measured need. Silent result expiry
and moving frame destruction into the middle of a batch are not substitutes
for that design decision.

`Nursery::next<T>()` is an observation awaitable, not a `Task`: it parks the
caller's own record, claims and reserves one matching result, and never creates
a nested execution. An already-retired result takes the inline fast path; a
cancelled wait releases its reservation, and a result reserved by a Handle is
not a next candidate. Completion claims remain in their original queue position
until consumed, so cancellation releases them without changing publication order.
A typed wake pass stops when one waiter finds no claimable result but one may still
become claimable: without a state change, later waiters of that type cannot succeed
either. Exhaustion still wakes every matching waiter. This avoids repeated failed
scans across all parked consumers; individual queue and pending-producer scans
remain linear, so it is not a constant-time type index.
A parked `next<T>()` keeps waiting in two cases: a producer is still running, or a
published result is currently reserved by another consumer. The second case is not
cosmetic — the reservation is handed back when that consumer is cancelled, and the
hand-back makes the result claimable again at its original queue position. Telling
the waiters behind it "exhausted" would orphan a result that is still in the queue,
which breaks the `while (auto outcome = co_await nursery.next<T>())` consumption
loop. Handing a reservation back therefore wakes the waiter list, and dropping a
consumed entry does too, so a waiter parked on a reservation that is consumed
rather than released still gets its exhaustion answer instead of sleeping on.
A still-running child whose pending outcome a Handle reserved is different: it will
never publish a result this queue may take, so it does not keep a `next<T>()`
waiter parked. A retired result reserved by a Handle is tracked like any other
reservation, because a Handle wait can be cancelled and return it.
A discarded child's bookkeeping stays alive until the
retirement callback has returned and is released at the next safe point, so
retirement needs no shared-pointer host lease.

### Coroutine API boundary

Application code names runtime capabilities and uses the values they return.
The implicit runtime context, loop and clock objects, coroutine frames,
registrations and scope waiter lists stay inside the runtime, which
implementations reach directly and share with each other. The application
surface is task ownership and structured nurseries (`task.hpp`, `nursery.hpp`,
`with_nursery.hpp`, `group_error.h`, `fwd.h`), the cancellation combinators (`scope.hpp`),
`ScopeOutcome` (`scope_outcome.hpp`), `CancelScope` and `coro::Cancelled`
(`cancel_scope.h`, `cancelled.h`), cancellable sleeps (`sleep.hpp`), loop time as
values (`now.hpp`, `time.h`), `checkpoint()`
(`checkpoint.hpp`), `offload` (`offload.hpp`), `MutexGuard`
(`async_mutex.hpp`, `mutex_guard.hpp`) and `Notification` (`notification.hpp`)
— all declared in `src/coro/` and
enumerated by the guard allowlist in `cmake/ArchitectureGuard.cmake`. The
allowlist admits more than `src/application/` uses today: `offload` and
`Notification` are reached only by the plugin host; `current_scope()` also
serves the bridge and non-yielding transport cancellation checks, and
`MutexGuard` only by `dns/resolver/doh.h`, `dns/resolver/dot.h` and
`http/session.h`.

Shared runtime implementation types — frames, promises, awaiters, waiter and
timer nodes, result storage, group bookkeeping, and loop/scope access helpers —
live in `src/coro/detail/` under `coro::detail`. Public classes
may keep private helper types and state inside the class. Public template
headers include the complete definitions they need; this does not require every
private implementation type to move into `detail/`. Application code must not
include, name or access runtime internals; other modules reach them only
through the host surface above.

`Loop` and `CancelScope` keep their scheduling, registration and waiter
bookkeeping private. `coro::detail::LoopAccess` and `coro::detail::ScopeAccess`
are the only ways in, so a caller cannot bypass the ready queue or leave a waiter
linked into a scope it does not own. The offload pool's third-party type stops
at `Loop::Pool` in `loop.cpp`, so including a task no longer compiles a thread
pool.

Host code that bridges foreign threads into the runtime — the plugin host over
the driver ABI — does not touch those internals either. Waiting and
notification are covered by the public `coro::Notification`: a single-waiter,
coalescing signal whose `wait()` parks on the caller's own scope and whose
`notify()` wakes through the ready queue without allocating. What remains in
`coro/host.hpp`, the second, narrower door, is what a host still cannot express
with public primitives: `CancelObserver` (an RAII hook that observes one
scope's cancellation while the coroutine waits on another, e.g. inside a
shield) and `NurseryToken` (a nursery ingress token read only on the loop).
The token becomes null when admission closes, before children finish draining;
late posted requests take the existing cancellation/rejection path. The
architecture guard rejects `coro/detail/` includes and `coro::detail` symbols
in every module outside `src/coro/`, so the waiter bookkeeping stays inside the
runtime for all layers.

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
or `Nursery::scope()`; `current_scope()` is the same borrow reached
implicitly. The driver gateway uses it for ABI cancellation translation;
TCP stream wrappers and aggregate socket operations use it to observe sticky
cancellation without adding a second yield before the underlying I/O. It is borrowed for the lifetime of that
combinator or group and used on the loop thread. Reading it is not a
checkpoint; cancelling it (`cancel()`) and re-observing cancellation
(`throw_if_cancelled()`) are explicit calls, and the latter is how the plugin
gateway turns a synchronous ABI abort into coroutine cancellation. The run
root borrows `work.scope()` and stores that borrow to cancel its work from a
sibling signal watcher. Work-Resources own publication of this borrow: their
post-drain destructor clears it before nursery scope destruction, including
partial-launch cleanup and cancellation. Result collection is not a drain.

**Checkpoints.** `checkpoint()` yields and checks cancellation explicitly; use
it in CPU loops. Sleep, fd waits, stop waits, mutex acquisition, offload and
transport I/O and group `next()` are checkpoints too. TCP wrappers and aggregate
reads/writes check cancellation without yielding; each underlying recv/send
keeps its checkpoint and cancellable fd wait. Empty-buffer and invalid-stream
returns still observe cancellation. Handle result waits are checkpoints; inline
`Task` composition propagates the child's result and cancellation rather than
adding a checkpoint. `current_time()` reads loop time without suspending.
Mutex, sleep and fd operations are reusable descriptors: each `co_await` creates
its own registration, so a completed or cancelled wait leaves no state in the
operation. Relative sleep deadlines are sampled at each await.

**Leaving the loop.** `coro::offload(fn)` runs `fn` on the pool and returns its
result through the loop. Its owned callable is invoked as an lvalue, including
the cooperative joined form. Cancellation is *abandon*: the await throws
`Cancelled` while the work packet finishes on its own. `offload` takes ownership
of its callable immediately, so a task created from a temporary callable may be
stored before it is awaited. Callable captures must own their data or refer to
objects that outlive the worker, even after cancellation.
`offload_joined(fn)` instead delays cancellation unwind until the worker actually
finishes, so resources borrowed by its callable may be local to the awaiting
operation. A no-argument callable cannot be interrupted once running; a callable
accepting `std::stop_token` receives a stop request and may cooperate. Stop
callbacks run at cancellation on the loop and must not block or re-enter it.
Admission is FIFO on the loop before BS submission, with capacity equal to the
configured worker count. Cancelling an unsubmitted job removes it and releases
its captures immediately, including for joined waits. Submitted jobs retain their
capacity until actual completion, even when abandoned; queued submitted jobs are
skipped when cancelled. Joined waits also report worker defects that occur during
cancelled cleanup; cancellation does not hide a defect from their owner.
After root completion, `run()` keeps pumping
the inbox until all submitted work and completion accounting have drained;
workers may finish loop hand-offs, but cannot start requests in a closed group.
An uncooperative indefinitely blocked worker still prevents run() returning:
there is no hard thread-kill or bounded-shutdown guarantee.

**Plugin bridge.** The synchronous v1 C ABI runs create/update/destroy in one
worker packet on one thread. A cycle owner is a discard child of the composition
root's long-lived group. It owns a shielded group containing the joined worker
and all live or retiring HTTP children. A separate loop controller remains
runnable while the worker is parked. Caller/root cancellation hooks latch the
cycle state and wake that controller without allocation or runtime reentry.
The controller cancels the worker wait and every live HTTP scope; submitted
synchronous work still drains to actual completion.

Host Services copies a request on the worker and posts it to the loop. Admission
checks the cycle's sticky cancellation and group lifetime token before starting
HTTP; requests copied after the caller's cancellation are rejected. Publication
of the HTTP scope repeats the latch check. Operation registration and scope
cancellation belong to the loop; the worker only reads the atomic cancellation
view. The cycle's live set holds weak references; the group and each request
own actual work, so retirement does not rely on erasing a shared-ownership ring.
Completed request registrations are removed; completed owner frames are
reaped rather than retained until shutdown.

Each exchange has a steady-clock `bridge_wait_budget` (5s), covering dispatch
and I/O. Its remaining duration becomes a loop-clock deadline without restarting
the budget. Worker expiry returns NETWORK_ERROR and requests cancellation; owner
exit drains the HTTP child even when the worker has already stopped waiting.
Cycle cancellation returns CANCELLED. The caller may leave immediately, but ABI
arenas, module/logger leases and worker capacity survive to real completion.
An active caller receives host defects; the owner retains its error boundary
until the subscriber consumes the result or cancels. An unobserved late host
defect reaches the root group's error boundary. Ordinary cancelled/operational unused outcomes
are retired. Direct Bridge exchanges without a cycle report INTERNAL_ERROR to
their worker and propagate the host defect to the bound group's error boundary.
Synchronous HTTP still occupies its v1 worker, and an uncooperative
plugin can prevent shutdown completing. HTTP/DNS transport dependencies use loop
I/O and must not require another worker in the same saturated pool.

HTTP keep-alive deadlines also use loop time and are checked before reuse, not
by a background expiry timer. ManualClock tests use in-memory transport fakes;
real workers and real sockets still require SystemClock for end-to-end timing.
Translating a duration does not make cross-thread execution deterministic.
Production HTTP exchanges are bounded by the application IP-source/DNS scopes
or the bridge scope. Per-address connect budgets include TLS handshake; they
and optional queue budgets do not bound a complete response read. A direct
client caller must wrap the whole exchange for a deadline, otherwise stalled
I/O waits until caller cancellation.

**Subdomain tasks.** Each subdomain is one long-lived coroutine that sleeps,
updates and repeats, reusing one `UpdateTask` across cycles and carrying only
its pacing state — update interval, last successful forced publication, cycle
start — as frame locals. A forced update stays due after missing addresses,
failed publication or cycle timeout; only successful forced publication commits
the force-update completion time. A provider retry therefore retries the
unfinished forced publication even when DNS already matches the local address.
There is no accumulated back-off state: each cycle recomputes its own
delay, a provider-supplied `retry_after` overriding the interval, anchored at
the failure when one is supplied and at the cycle start otherwise. Each cycle
is bounded by its own timeout, with a shorter one around the DNS read, so a
dead resolver surfaces in seconds. A subdomain task exits under cancellation:
`coro::Cancelled` propagates out of the pacing sleep and the task logs it
and rethrows, so the group's cancel ends the work phase. One scheduling loop
owns both normal cycles and retries. A defect escaping a cycle is logged and
retried after one full update interval; the next cycle is forced when force
updates are enabled, preserving the former restart policy. A defect therefore
never retires the subdomain, and a non-positive configured
interval is clamped to a minimum rather than spinning.

### Blocking-operation inventory

Network waits do not park the loop thread: sockets are opened with `O_NONBLOCK`
(`network/transport/socket_ops.cpp`), so connect/send/recv wait on readiness when
needed; SSL calls await the direction indicated by WANT_READ/WANT_WRITE. DNS,
provider HTTP and the 2s mDNS window all use cancellable loop I/O. This is not
a guarantee that every system or library call on the loop has zero latency.
File/loader I/O runs at startup and plugin ABI work uses offload. The sanctioned
randomness calls below remain synchronous and have no strict wall-time bound.
Runtime network-interface metadata uses owned offload packets; socket setup and
I/O remain on the loop. Default IPv4 mDNS uses INADDR_ANY without metadata and
therefore does not wait for worker capacity. Startup/CLI metadata queries before run remain synchronous.

| Blocking/synchronous point | Where | Limits and caveats |
|----------------------------|-------|--------------------|
| `getifaddrs()` | `infrastructure/ip_source/iface_util.cpp`, offload for runtime interface/mDNS sources; synchronous before run | Live owned snapshot with IPv6 scope IDs, no cache; the caller deadline covers admission and work. Abandoned packets own all inputs until completion |
| interface name/index lookup | `infrastructure/ip_source/mdns.cpp` and `iface_util.cpp`, offload for runtime selection | Index/address/name are computed before socket setup; no socket is passed to the worker. IPv4 default routing and IPv6 interface filtering are unchanged |
| random acquisition | `support/util/random.hpp`, called by DNS builders and dispatcher shuffling on the loop | RAND_bytes may initialize/reseed or synchronize; random_device seeds the thread-local mt19937 on first use. No guaranteed latency bound. Unsent dispatcher preflight, fixed-ID mDNS and DoT wire sizing bypass secure-ID acquisition; cryptographic failure is an operational DNS error, never a predictable fallback |
| startup file/loader I/O | `composition/assembly.cpp` and `composition/commands/`, on the main thread before `coro::run` | Config read, static validation, plugin `dlopen`, the environment check and the trust-context build happen once, before any coroutine exists |
| `/etc/resolv.conf` read | `infrastructure/dns/bootstrap/resolv_conf.cpp`, reached from `composition/assembly.cpp` on the main thread | A `std::ifstream` of a small system file, read once at startup to fill the bootstrap server list when none is configured |
| CA discovery + trust-store load | `infrastructure/network/tls/context.cpp`, off-loop only (`TlsContext::create`) | Bundle discovery is a function-local static, so the filesystem is touched once per process rather than per `create()`; trust material is then populated eagerly during `create()` through `SSL_CTX_load_verify_locations`, or eager `X509_STORE_load_file` for each hashed certificate/CRL entry in the default cert dir. Trusted PEM AUX is preserved; missing/empty directories or any unreadable, malformed or hash-mismatched indexed entry reject the entire snapshot (stricter than lazy lookup). No lazy hash-dir lookup is installed, and existing contexts keep their snapshot until rebuilt. One immutable context is shared; handshake verification reads the trust snapshot instead of the filesystem |
| plugin ABI cycle (`create`/`update`/`destroy`) | offload pool; cycles of one driver run concurrently on distinct instances, bounded by the pool's worker count | The C ABI is synchronous, so the cycle runs on a worker and waits until the bridge's HTTP child coroutine completes on the loop |
| log write | log drain thread (async spdlog sink) | The caller hands the record over and continues; a full queue discards the newest record |
| CPU-intensive work | any `coro::offload` call site | `offload` is the single exit for blocking *and* CPU-bound work (the `asyncio.to_thread` analogue); the total off-loop workload is structurally bounded by the configuration |

`src/support/util/` holds generic helpers — algorithm search, byte order,
file-descriptor ownership, randomness and input validation. Randomness is the
synchronous exception recorded above, not an absolute non-blocking guarantee.
Other blocking or CPU-intensive additions must use `coro::offload`. Do not
replace CSPRNG transaction IDs with mt19937 to avoid entropy cost.

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
environment validator, `yaddnsc_coro_application` the main loop, the subdomain
task and the update task. `yaddnsc_coro` is a base module beside
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
  scopes, cancellation combinators, sleeps, `AsyncMutex`, `offload`, and process stop signals.
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
- `include/yaddnsc/sdk/signing.hpp`: header-only crypto helpers for plugins
  (`yaddnsc_plugin_sdk_crypto` interface target). Consumers compile the helpers
  and link OpenSSL; no precompiled C++ objects cross toolchain boundaries.

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

## Manual boundary review

The `architecture_guard` ctest (`cmake/ArchitectureGuard.cmake`) checks textual
boundaries — includes, layering, threads/futures and the removed cancellation
token — but it cannot judge every loop-safety decision. Loop safety itself (no
blocking work on the loop thread, including OpenSSL's lazy trust loaders) is
reviewed by hand; see the
[blocking-operation inventory](#blocking-operation-inventory).

Beyond the application-layer checks described below, the same script also
enforces, in `cmake/ArchitectureGuard.cmake`:

- lower layers stay below their public capability: `src/domain/` may not include
  `infrastructure/`, Glaze, spdlog, CLI11 or cancellation-token headers, and
  `src/coro/` may not include spdlog (loop trace output goes out through the
  injected `Loop::TraceSink`);
- `network/transport/` may not include the TLS or factory internals, and
  `http/` may not include concrete `dns/` backends — both take an injected
  capability instead;
- plugins are SDK-only: no host `src/` modules and no legacy utility headers
  (`core_logger*`, `CORE_LOG*`, `xml_raii.h`, `form_encode.h`, `uri.h`,
  `fmt.hpp`, `string_util.hpp`, `HttpClient*`, `spdlog/`), since logging
  travels through Host Services;
- `include/yaddnsc/sdk/driver_abi.h` may include only C standard headers
  (`stddef.h`, `stdint.h`, `stdbool.h`);
- no parallel HTTP types: no `HttpTransport`, `HttpRequest` or `HttpResponse`
  definitions outside the plugin SDK's own ABI DTOs;
- host `src/` code may not include `yaddnsc/sdk/driver.hpp` — the host speaks
  `driver_abi.h` only;
- no `-rdynamic`, `export-dynamic` or `dynamic_lookup` in any CMake file, so
  plugins cannot resolve host symbols (`ENABLE_EXPORTS` is deliberately not
  matched);
- `rules/01`–`rules/04` stay fact-free: a backticked project path, an
  `ns::symbol`, a `YADDNSC_*`/`YLOG_*` constant or a `*_BUDGET` constant in a
  normative rule fails the build, which keeps those documents from restating
  project state.

For `src/application/`, the guard allows direct coroutine includes only from
`async_mutex.hpp`, `cancel_scope.h`, `cancelled.h`, `checkpoint.hpp`, `fwd.h`,
`group_error.h`, `mutex_guard.hpp`, `now.hpp`, `nursery.hpp`, `offload.hpp`, `scope.hpp`,
`scope_outcome.hpp`, `sleep.hpp`, `task.hpp`, `time.h` and
`with_nursery.hpp`. These coroutine include and internal-access checks apply only to
`src/application/`; the thread/future include check is wider and globs
`src/application/`, `src/infrastructure/` and `src/composition/` together.
For application code, it rejects explicit internal types/context, `coro::detail`,
coroutine namespace imports, `promise_type` access and waiter/context-management
methods. Simple
namespace aliases (including chains) and split qualifiers are normalized.
String literals are blanked out before comments are stripped, so a `//` inside a
URL does not truncate the rest of the line and hide a violation; block and line
comments are otherwise ignored for these symbol checks. Public scope
cancellation/state reads (`cancel()`, `cancelled()`, `timed_out()`,
`throw_if_cancelled()`) and unrelated `release()` calls remain allowed.
Application symbol checks also reject references, imports, namespace reopening
and simple aliases of infrastructure namespace roots (`net`, `dns`, `http`,
`ipsource`, `Config`, `configuration`, `logging`, `plugin`, `infrastructure` and
`Utils::Cert`), including nested/split qualifiers. An alias is rejected at its
infrastructure target, so an alias chain does not need to be resolved. Keep this
root list in the guard aligned with new infrastructure namespaces.
To catch concrete-type forward declarations, the guard harvests ordinary
`class`/`struct` definitions throughout infrastructure `.h`/`.hpp` headers
(excluding the coroutine tree), then rejects matching application forward
declarations outside brace blocks. For this name check only, innermost brace
blocks are iteratively replaced with markers: declarations in `app`/`domain`
namespace blocks may share infrastructure names, such as `app::Stream` versus
`net::Stream` or `domain::SubdomainConfig` versus `Config::SubdomainConfig`.
Infrastructure namespace checks still inspect the original normalized text,
including references inside business namespace blocks. Thus the old `class NetworkInterfaces;`
dependency in `diagnostics.h` and newly named implementation classes cannot
bypass the include boundary merely by omitting the implementation include.
Definitions, not borrowed forward declarations, populate this inventory;
application/domain declarations and public coroutine APIs remain allowed.

Pure contract headers under `src/application/ports/` (`.h`/`.hpp`, including
subdirectories) have a narrower include boundary: internal includes may target
only `domain/`, `application/ports/` or the public coroutine header whitelist
above. All quoted includes and angle includes using known source-root paths are
checked as internal; bare/relative quoted paths and traversal components are
rejected. Standard-library angle includes remain allowed except `<format>`;
`fmt/...` includes are also rejected. This blocks `support/fmt.hpp` and
`application/log_macros.h` from pulling formatting conveniences back into contracts.
Function-like `#define NAME(...)` declarations are rejected in port headers;
object-like macros, including include guards, remain allowed. This does not
restrict the number of ports per header or legitimate contract aggregation.
Ordinary application headers outside `ports/`, including the macro convenience
layer `application/log_macros.h`, may still use support helpers and formatting.

`test_architecture_guard_cases` runs the actual CMake script on isolated source
fixtures, and `test_coro` checks that raw Task/group/Handle construction, Task
context/frame access, and the loop's scheduling/registration services and the
scope's waiter bookkeeping are unavailable to callers.

These are textual checks: they do not resolve C++ types, all macros/aliases,
relative or macro-generated includes, string literals, or arbitrary template
metaprogramming. The concrete-type inventory is a conservative name heuristic,
not scope resolution: it also collects namespaced/nested definitions. Namespace
checks likewise match names without resolving ownership; an unrelated nested
namespace with a reserved infrastructure root name can be rejected.
Macro/attribute-generated or otherwise nonordinary declarations, and
implementation types exposed only through transitive includes or deduction still
require review. Port include/macro checks are line-based: they do not expand
macros or backslash-spliced directives, trace transitive includes, or fully lex
multiline comments; review must check those forms too. The brace-block reduction
is not C++ scope analysis: declarations inside any brace block are excluded from
the inventory name check, and unmatched/macro-generated braces or nonordinary
qualified declarations require review. A same-name declaration outside brace
blocks is still conservatively rejected without proving infrastructure ownership;
this policy does not ban same-name `app`/`domain` declarations in namespace blocks.
Comment/string normalization is not a complete C++
lexer (notably escaped quotes and raw strings), and inactive preprocessor branches
are still scanned. Public headers necessarily include implementation definitions;
review must still check inferred objects and callbacks for runtime access.
The normative boundary is in
[Coroutine API boundary](#coroutine-api-boundary).

When touching startup, transport or IP-source code, confirm by hand:

- **Runtime interface metadata reads leave the loop.** Interface and mDNS
  sources read live snapshots and name/index metadata through owned
  `coro::offload` packets, sharing admission with plugin workers. The enclosing
  timeout covers admission and reading; default IPv4 mDNS uses INADDR_ANY
  without metadata or worker admission. Startup/CLI listings stay synchronous.
  Do not add a cache, mutex or single-flight wait. Keep blocked-provider and
  pool-saturation regressions, and measure loop responsiveness as well as lookup
  wall time when changing this boundary.
- **A CA bundle is only ever loaded off the loop.** Build trust material with
  `net::TlsContext::create` before `coro::run` or from `coro::offload`. A stream
  receives a pre-built context and must never load a CA bundle or register a lazy
  verify path during a handshake.
- **New blocking primitives do not enter a production loop path.**
  `src/support/` is a generic helper layer; random_device initialization and
  OpenSSL RAND calls are synchronous exceptions, not guaranteed non-blocking.
  Cryptographic failure must not fall back to the non-cryptographic PRNG. A new
  cache or retry helper that takes a lock or sleeps does not belong on a
  coroutine path. Route other blocking or CPU-bound work through `coro::offload`
  — see the [blocking-operation inventory](#blocking-operation-inventory).
- **Clock domains do not cross as absolute timestamps.** Worker future waits
  use real time; bridge scopes translate only the remaining duration into loop
  time. Session keep-alive uses loop time. Use fake transports with ManualClock
  and SystemClock for real-worker/socket tests; an auto-jumping clock cannot
  make external progress deterministic.
- **Transport simplification preserves cancellation.** Empty buffers and
  invalid streams must still observe an already cancelled scope. Keep yielding
  cancellation points in the underlying I/O, not redundantly in every wrapper.
  The `bench_tcp_prefix` target measures buffered two-byte prefixes through the
  actual TCP stream; use a separate Release build for timing, not sanitizer
  results:

  ```bash
  cmake -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DYADDNSC_BUILD_TESTS=ON
  cmake --build build-perf --target bench_tcp_prefix --parallel
  ./build-perf/test/perf/bench_tcp_prefix --benchmark_repetitions=5
  ```

## Plugin boundary (v1)

Drivers are runtime-loaded shared libraries talking to the host exclusively
through the **v1 C ABI** (`include/yaddnsc/sdk/driver_abi.h`) plus an
optional C++ helper layer (`include/yaddnsc/sdk/driver.hpp`). What crosses the
`.so` boundary is C-representable: fixed-width integers, length-delimited borrowed
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
  host dlsym-probes it and reports the answer through
  `GatewayPort::supports_validate`, so `driver_params` can be checked against
  the driver's schema before any update is published. Without it the plugin
  provides no such check: `config test` fails, and `run` warns and continues
  with the parameters unchecked. The plugin still loads and can update either
  way, so a driver built against an older ABI keeps working.
- Capability bits are enforced before the plugin runs: `A` and `AAAA` are
  delivered when the matching bit is set, and a record type outside that set
  ends as `DriverError::UPDATE_FAILED`, a host-side decision that produces no
  plugin ABI status.
- Each update runs on a fresh driver instance (`create → update → destroy`).
  Instances of the same module may update concurrently, and each instance
  serves one update at a time. Each instance runs its own `create()`,
  independent of process lifetime. The loader fixes one execution group per
  module: the gateway drives the sync group on an offload worker with the
  sync table, or the async group with loop-side HTTP tasks and the async
  table. An async wait therefore does not hold a worker.
- Host Services contexts track create/update/validate/destroy on the cycle's
  worker. HTTP is accepted only during update, even through saved service-table
  copies. Contexts are destroyed after the cycle and must not cross cycles.
  Inputs/log arguments expire on return to the sender; returned errors are
  copied before the next same-thread ABI operation or destruction; HTTP response
  arenas last through update, and descriptors last while the module is loaded.
- Provider HTTP, logging and cancellation reach the plugin through Host
  Services: the host owns the HTTP client (reached through the bridge) and the
  log sink, forwarding source location to `spdlog::source_loc`. ABI 1.1 adds
  an optional `log_enabled` predicate in the host-services extension container;
  the SDK macros evaluate the level, query it, then skip argument evaluation
  and formatting when it is disabled (without a log callback neither is
  evaluated). The host still filters at `log` and never lets a query defect
  fail an update.
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
