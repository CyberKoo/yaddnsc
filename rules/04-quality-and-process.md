# Quality & Process

**Requirement levels:** **must / must not** are hard requirements; **prefer / normally** describe defaults; **may** permits an explicit exception under the stated conditions. Build commands and current enforcement belong in [Development](../docs/development.md), not this guideline.

## Concurrency

- Shared mutable state **must** have a defined synchronization strategy; public interfaces **must** document their thread-safety guarantees and caller obligations.
- Concurrency **must** go through the project's coroutine runtime rather than raw threads, futures or thread pools. Blocking or CPU-bound work **must** leave the loop through the runtime's offload primitive. The thread model, the startup and foreign-function hand-offs that sit outside it, and the sanctioned blocking points are recorded in [Concurrency & I/O model](../docs/architecture.md#concurrency--io-model) and the [blocking-operation inventory](../docs/architecture.md#blocking-operation-inventory); what the guard does and does not prove about them lives in [Manual boundary review](../docs/architecture.md#manual-boundary-review).
- Waiting **must** be a cancellable await, so scope cancellation reaches it; a deadline **must** wrap the operation as a cancel scope rather than travel down as an I/O parameter. Signal handling **must** go through the runtime's signal facility rather than a dedicated thread.
- **Prefer** explicit ownership and dependency injection over global mutable state. Global/singleton ports **must not** be introduced (see [Function & Constructor Signatures](02-implementation.md#function--constructor-signatures)).
- If non-port process-wide state is unavoidable, initialization **may** use a function-local static (Meyers singleton) for one-time construction, or `std::call_once` for a separate initialization operation. Neither protects subsequent mutation. `constinit` guarantees static initialization, not immutability or thread safety; access to mutable state still **must** be synchronized.

## Security

- External input **must** be validated at system boundaries using runtime checks, not assertions; see [Contracts & Assertions](03-error-handling.md#contracts--assertions).
- Buffer accesses and slices **must** respect bounds; non-owning views **must** remain within the lifetime of their backing storage. **Prefer** `std::span` / `std::string_view` for suitable interfaces, but neither provides automatic bounds checking or lifetime management. C-string interoperability **must** follow [String Handling](02-implementation.md#string-handling).
- Secrets and personal data **must not** appear in logs; diagnostic values **may** be included only after redaction that removes sensitive content.

## Testing

- Tests **must** cover changed behavior and its failure paths. **Prefer** virtual interface-based mocks for external dependencies and integration tests for component boundaries. The framework, mocking approach and test naming convention are fixed for this project; see [Tests](../docs/development.md#tests).
- Relevant tests **must** pass before merge; available suites, commands, and CI coverage are documented in [Development](../docs/development.md#tests).
- **Normally** aim for at least **80% line coverage**, prioritizing meaningful branch coverage on critical paths rather than tests written only to raise the percentage. This is a quality target, **not an implemented percentage gate**; reporting and enforcement status live in [Coverage](../docs/development.md#coverage).
- Changes to the plugin ABI **must** follow the bump classification and minor-bump checklist in [Plugin ABI changes](../docs/development.md#plugin-abi-changes); the frozen ABI compatibility test **must** stay green, and the frozen baseline header **must not** be edited.

## Logging

Diagnostic logging **must** use the project's centralized logging system. Each layer has exactly one designated entry point, tabulated in [Layers](../docs/architecture.md#layers); the facade requirement in [Components That Must Be Reused](02-implementation.md#components-that-must-be-reused) does not require injecting the application port into every layer. Ad-hoc loggers and diagnostic output via `std::cout`, `printf`, or `std::print` **must not** bypass this system. User-facing CLI output is presentation, not logging, and **must not** carry debug traces or internal-state warnings.

The central production backend **must** supply timestamp, severity, source location, and message; SDK source locations **must** be forwarded through Host Services. A log call **must** return without blocking the calling (loop) thread: the backend is asynchronous and drained on its own thread (see [Concurrency & I/O model](../docs/architecture.md#concurrency--io-model)). See [Plugin boundary](../docs/architecture.md#plugin-boundary-v1) for ABI boundaries.

### Severity Semantics

Severity **must** classify the event's consequence for the process, and the same event **must** carry the same severity at every layer where it is logged:

| Level | Meaning |
|-------|---------|
| TRACE | Detail of a single request or I/O operation; high volume, useful only while following one interaction |
| DEBUG | Per-iteration or component lifecycle events (an attempt, a reconnect, a skipped step with its reason); the narrative of normal operation |
| INFO | Startup, shutdown, and successful completion of the primary periodic work; state an operator checks |
| WARN | Self-healing degradation: operation continues through retry, fallback, or clamping without operator action |
| ERROR | The current unit of work failed and does not recover on its own — its result is lost, operator action (credentials, configuration) is required, or a subsystem crashed unexpectedly |
| CRITICAL | Process termination is imminent |

Expected transient conditions handled by design (timeouts, empty results, fallback, rate limiting) are **WARN**, not ERROR. An unexpected crash, or a condition that persists until someone intervenes, is **ERROR**.

### Message Text

- The format string **must** be a string literal, so placeholder and type mismatches are compile-time errors.
- A message is a single concise statement: ASCII-only, first letter capitalized, and **must not** end with a period; an in-progress action **may** end with `...`. A message **may** instead begin with a component, interface, or configuration token in its native spelling (including lowercase) as a prefix.
- Hostname, FQDN, and URL values **must** be double-quoted (`"{}"`); plugin-identity tokens (names, symbols, library paths) and configuration literals use single quotes (`'{}'`); numbers, host:port pairs, and enum names stay bare.
- Error detail **must** be appended last as `: {}`; qualifiers (status codes, subsystems) belong inside the sentence, not in a parenthesized suffix.
- Payload bodies **may** be logged only truncated to a fixed byte budget; sensitive content remains subject to [Security](#security).

### Lazy Argument Evaluation

A log argument with non-trivial cost or side effects **must** be evaluated only when its level is enabled — guard the call with the layer's severity query or use a macro that formats lazily. Macros that forward arguments into a formatting call evaluate them before the level check, so the cost and side effects otherwise happen on every call.

## Documentation

- Public APIs **must** document contracts, ownership/lifetimes, failure behavior, and thread-safety obligations where applicable, using Doxygen-style `/** ... */` or `///` comments. **Prefer** comments explaining constraints and intent over restating code.
- Source files **must not** contain IDE-generated author/date banners such as `Created by ...`. **Prefer** no file-level banner; a concise file comment **may** explain non-obvious responsibility, boundaries, or constraints, but **must not** merely restate the filename or declarations.
- Planned improvements **may** be recorded as `// TODO(username): description`; TODOs **must not** replace required correctness or safety work.

## Performance Optimization

- **Prefer** clarity; optimization **normally** follows profiling that identifies a bottleneck. Performance claims **must** be supported by measurements.
- Returning a named local eligible for NRVO **must not** use `return std::move(local)`, which prevents NRVO. An explicit move from a member or dereferenced owner **may** be appropriate when ownership transfer is intended and the source's remaining lifetime/use permits it.
- **Prefer** `reserve()` when the final container size is known and preallocation is useful.
- Critical-path changes **normally** include benchmarks in the dedicated suite. Historical trend tracking is a performance goal, not a claim of implemented baselines or regression gates; current benchmark/CI behavior is recorded in [CI and warning gates](../docs/development.md#ci-and-warning-gates).

## Cross-Platform Development

- Platform-specific implementation **must** follow [Components That Must Be Reused](02-implementation.md#components-that-must-be-reused) and remain outside public headers. **Prefer** the standard library and `std::filesystem` when they meet requirements.
- Platform-sensitive changes **must** identify affected supported targets and record validation performed or gaps requiring follow-up. Current platform jobs and their limitations are documented in [CI and warning gates](../docs/development.md#ci-and-warning-gates); their existence does not imply an all-platform merge gate.

## Version Control & Collaboration

- Releases **must** follow **Semantic Versioning** (`MAJOR.MINOR.PATCH`).
- Commit messages **must** use **`<Type>: <description>`**, with a capitalized imperative description, e.g. `Fix: Harden URI port parsing against malformed values`. Scope prefixes such as `fix(core):` **must not** be used. The type **must** be one of:
  - `Fix:` — corrects wrong behavior.
  - `Feat:` — adds a user-visible capability.
  - `Perf:` — improves performance without changing behavior.
  - `Refactor:` — restructures code without changing behavior; not merely formatting.
  - `Style:` — formatting/whitespace only; no semantic change at all.
  - `Docs:` — documentation only.
  - `Test:` — adds or fixes tests without touching production code.
  - `Build:` — build system, compiler configuration, or toolchain.
  - `CI:` — CI workflows and automation configuration.
  - `Deps:` — adds, removes, or bumps a dependency.
  - `Revert:` — reverts a previous commit; the subject names the reverted change.
  - `Chore:` — other maintenance that fits none of the above.
- **Prefer** commit messages that include a body summarizing changes as `- ` bullet points, separated from the subject by a blank line. Bullet points **must not** carry implementation details (function/type names, line-level edits, internal control flow); they state **what** changed and **why** from the reviewer's perspective — the diff already records the **how**. A trivial one-line change (typo, rename, comment) **may** have a subject only.
- **Prefer** small, focused PRs (normally fewer than 400 changed lines) with linked issue tickets when applicable. Larger changes **may** remain together when splitting would obscure a cohesive change; explain the scope.
- **Normally** squash-and-merge to mainline for a linear, bisectable history.

## Documentation Maintenance

- Changes to behavior, APIs, dependencies, or build steps **must** update the relevant owner document in the same change; unrelated documentation **must not** be churned. See [Documentation ownership](../docs/architecture.md#documentation-ownership).
- README files **must** be updated when a user-visible change affects their content, not for every internal change. Build/test/CI details belong in `docs/development.md`; provider parameters and SDK/ABI details belong in their respective owner documents.
- When an affected document has language counterparts (e.g. `README.md` / `README_CN.md`, `DRIVERS.md` / `DRIVERS_CN.md`), all counterparts **must** be updated together with equivalent content. A language discrepancy is a documentation bug.
- Adding, removing, or renaming a rules topic **must** update the relevant numbered rules file and the index in [`AGENTS.md`](../AGENTS.md) in the same change.
- A normative rule states a discipline that holds in any C++ project. It **must not** name this project's source paths, layer directories, types, macros or budget constants; a rule that needs one states the principle and points at the owner document instead. Project facts — which layer owns which entry point, which framework a test tier uses, where a component lives — belong in `docs/`, so that a rename changes one document rather than every rule that mentions it. The `architecture_guard` check for this rule covers the path and symbol forms it can see; prose review covers the rest.
- [Examples](05-examples.md) are non-normative and **may** name real files, because pointing at the actual interface is their purpose.

## Code Review Checklist

Every review **must** check the applicable items below and identify validation gaps rather than claiming unrun checks passed:

- [ ] Existing components and dependencies are reused per [Reuse Protocol](02-implementation.md#reuse-protocol) and [Contribution & Duplication Rules](02-implementation.md#contribution--duplication-rules).
- [ ] Ownership, RAII, moves, and view lifetimes follow [Memory & Resource Management](02-implementation.md#memory--resource-management) and [String Handling](02-implementation.md#string-handling).
- [ ] Interfaces and dependency injection follow [Const Correctness](02-implementation.md#const-correctness) and [Function & Constructor Signatures](02-implementation.md#function--constructor-signatures).
- [ ] Error contracts, exception safety, `noexcept`, and catch boundaries follow [Error Handling](03-error-handling.md), particularly [Exception Safety & noexcept](03-error-handling.md#exception-safety--noexcept) and [Exception Discipline & Catch Block Rules](03-error-handling.md#exception-discipline--catch-block-rules).
- [ ] External validation, bounds, and side-effect-free assertions follow [Security](#security) and [Contracts & Assertions](03-error-handling.md#contracts--assertions).
- [ ] Shared-state synchronization and documented thread-safety follow [Concurrency](#concurrency).
- [ ] Logging respects the [Logging](#logging) policy — layered entry points, severity semantics, message text, lazy evaluation — CLI output separation, and sensitive-data restrictions.
- [ ] Relevant tests cover changed behavior and failure paths; results and platform-validation gaps are recorded, and performance claims have measurements.
- [ ] Relevant owner documentation and its language counterparts are synchronized per [Documentation Maintenance](#documentation-maintenance).
