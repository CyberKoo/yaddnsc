# Runtime flow

The business update cycle, described at the level of tasks and mechanisms;
the code is the record of its implementation. Structural boundaries are in
[architecture.md](architecture.md); the concurrency contracts behind them in
[concurrency.md](concurrency.md).

```text
argv
  |
  v
startup (main thread, before any coroutine exists)
  config load and validation, plugin loading, resolver bootstrap,
  trust material, environment checks
  |
  v
loop
  |
  +-- root scope (fail-fast; owns the long-lived services)
  +-- stop watcher — awaits the process stop request
  +-- work group (supervised; cancelled by the stop watcher)
        +-- startup validation of every record's driver parameters
        +-- one long-lived task per subdomain
              +-- update cycle: acquire address -> read current record
                    -> decide -> push provider update
```

- **Startup** runs entirely on the main thread before the loop starts, so
  blocking one-shot work (file I/O, plugin loading, trust material) never
  touches the loop. Each blocking startup step checks the stop latch after
  completing, so a graceful stop still lands between steps.
- **The work group isolates failures.** A failing subdomain does not take its
  siblings down; a defect at the root cancels the run. The loop reports an
  outcome and the command maps it to an exit status: a stop request is the
  normal exit, while a rejected configuration and the unexpected end of all
  update tasks are failures.
- **Startup validation.** Every record's driver parameters are checked through
  the plugin's validation entry before any update task starts, under a
  per-record budget, with identical records validated once. A driver that
  rejects its parameters fails startup. A driver without a validation entry
  is warned about and skipped under `run` — refusing to start would break a
  configuration that updates correctly — but fails `config test`, whose whole
  purpose is answering "is this configuration good?".
- **Stop** is a single latched facility. The first stop request begins a
  graceful drain — in-flight plugin work finishes before the loop returns;
  a second request terminates the process immediately. Normal shutdown drains
  submitted workers and pending logs; forced termination may lose queued
  diagnostics but never waits for them.
- **Diagnostic commands** run on the same runtime with a short-lived loop of
  their own, bounded by their own deadlines.

## The update cycle

Each subdomain is one long-lived task that sleeps, updates and repeats,
carrying only its pacing state. A cycle acquires the local address, reads the
current DNS record, decides whether a change is needed, and pushes the
provider update through the driver.

- **Fail-open decision.** A lookup error does not suppress the provider
  update — pushing an unchanged record is harmless, skipping a changed one is
  not. Skipping the update requires a definitive match.
- **Resolver strategy.** The resolver sequences or races its configured
  backends, fails over on operational errors, and treats negative answers as
  definitive; one endpoint's diagnosis never hides a sibling's operational
  failure, and only whole-query operational failures are retried.
- **Pacing and retry.** Each cycle is bounded by its own budget, with a
  shorter one around the DNS read so a dead resolver surfaces quickly. There
  is no accumulated back-off state: a failure retries after the normal
  interval, a provider-supplied retry hint overrides it, and a forced update
  stays due until it actually publishes.
- **A defect inside a cycle is logged and retried** after a full interval; it
  never retires the subdomain task.
