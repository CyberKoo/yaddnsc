# Quality & Process

**Requirement levels:** **must / must not** are hard requirements; **prefer / normally** describe defaults; **may** permits an explicit exception under the stated conditions. Build commands and current enforcement belong in [Development](../docs/development.md), not this guideline.

## Concurrency

- Shared mutable state **must** have a defined synchronization strategy; public interfaces **must** document their thread-safety guarantees and caller obligations.
- **Prefer** explicit ownership and dependency injection over global mutable state. Global/singleton ports **must not** be introduced (see [Function & Constructor Signatures](02-implementation.md#function--constructor-signatures)).
- If non-port process-wide state is unavoidable, initialization **may** use a function-local static (Meyers singleton) for one-time construction, or `std::call_once` for a separate initialization operation. Neither protects subsequent mutation. `constinit` guarantees static initialization, not immutability or thread safety; access to mutable state still **must** be synchronized.

## Security

- External input **must** be validated at system boundaries using runtime checks, not assertions; see [Contracts & Assertions](03-error-handling.md#contracts--assertions).
- Buffer accesses and slices **must** respect bounds; non-owning views **must** remain within the lifetime of their backing storage. **Prefer** `std::span` / `std::string_view` for suitable interfaces, but neither provides automatic bounds checking or lifetime management. C-string interoperability **must** follow [String Handling](02-implementation.md#string-handling).
- Secrets and personal data **must not** appear in logs; diagnostic values **may** be included only after redaction that removes sensitive content.

## Testing

- Tests **must** use **GoogleTest**, with **GoogleMock** as the sole mocking framework; alternatives **must not** be introduced. Test names **must** follow `TEST(ClassName, MethodName_Scenario_ExpectedBehavior)`.
- Behavior changes and bug fixes **must** have relevant tests, including failure paths. **Prefer** virtual interface-based mocks for external dependencies and integration tests for component boundaries.
- Relevant tests **must** pass before merge; available suites, commands, and CI coverage are documented in [Development](../docs/development.md#tests).
- **Normally** aim for at least **80% line coverage**, prioritizing meaningful branch coverage on critical paths rather than tests written only to raise the percentage. This is a quality target, **not an implemented percentage gate**; reporting and enforcement status live in [Coverage](../docs/development.md#coverage).

## Logging

Diagnostic logging **must** use the project's centralized logging system through the layer-specific entry points below. The facade requirement in [Components That Must Be Reused](02-implementation.md#components-that-must-be-reused) does not require injecting the application port into every layer. Ad-hoc loggers and diagnostic output via `std::cout`, `printf`, or `std::print` **must not** bypass this system.

### Layered Logging Policy

| Layer | Required boundary / permitted entry point |
|-------|-------------------------------------------|
| **Domain** (`src/domain/`) | **Must not** perform logging or depend on logging ports/backends; diagnostics belong to callers. |
| **Application** (`src/application/`) | **Must** use the injected `Logger` port and `YLOG_*` macros from `src/application/ports/log.h`; **must not** call spdlog directly. |
| **Infrastructure / support** (`src/infrastructure/`, `src/support/`) | **May** use `SPDLOG_*` / spdlog directly through the centrally configured backend. **Must not** create independent sinks or global/singleton ports, or introduce an application `Logger` dependency solely for logging. |
| **SDK / plugins** (`include/yaddnsc/sdk/`, `driver/`) | **Must** route diagnostic logs through Host Services (`yaddnsc_host_services::log`); C++ helpers **normally** use `YADDNSC_SDK_LOG_*` from `include/yaddnsc/sdk/driver.hpp`. **Must not** depend on host-internal logging headers or call spdlog directly. |
| **CLI / composition** (`src/cli/`, `src/composition/`) | Host-adapter diagnostics **may** use the centrally configured spdlog backend. User-facing CLI output is not logging and **normally** uses `std::print` / `std::println` on stdout/stderr; debug traces and internal-state warnings **must not** be presented as command output. |

The central production backend **must** supply timestamp, severity, source location, and message; SDK source locations **must** be forwarded through Host Services. See [Layers](../docs/architecture.md#layers) and [Plugin boundary](../docs/architecture.md#plugin-boundary-v1-alpha) for dependency and ABI boundaries.

## Documentation

- Public APIs **must** document contracts, ownership/lifetimes, failure behavior, and thread-safety obligations where applicable, using Doxygen-style `/** ... */` or `///` comments. **Prefer** comments explaining constraints and intent over restating code.
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
- Commit messages **must** use **`<Type>: <description>`** (`Fix:`, `Feat:`, `Docs:`, `Refactor:`, `Test:`, `Chore:`), with a capitalized imperative description, e.g. `Fix: Harden URI port parsing against malformed values`. Scope prefixes such as `fix(core):` **must not** be used.
- Commit messages **must** include a body summarizing changes as `- ` bullet points, separated from the subject by a blank line.
- **Prefer** small, focused PRs (normally fewer than 400 changed lines) with linked issue tickets when applicable. Larger changes **may** remain together when splitting would obscure a cohesive change; explain the scope.
- **Normally** squash-and-merge to mainline for a linear, bisectable history.

## Documentation Maintenance

- Changes to behavior, APIs, dependencies, or build steps **must** update the relevant owner document in the same change; unrelated documentation **must not** be churned. See [Documentation ownership](../docs/architecture.md#documentation-ownership).
- README files **must** be updated when a user-visible change affects their content, not for every internal change. Build/test/CI details belong in `docs/development.md`; provider parameters and SDK/ABI details belong in their respective owner documents.
- When an affected document has language counterparts (e.g. `README.md` / `README_CN.md`, `DRIVERS.md` / `DRIVERS_CN.md`), all counterparts **must** be updated together with equivalent content. A language discrepancy is a documentation bug.
- Adding, removing, or renaming a rules topic **must** update the relevant numbered rules file and the index in [`AGENTS.md`](../AGENTS.md) in the same change.

## Code Review Checklist

Every review **must** check the applicable items below and identify validation gaps rather than claiming unrun checks passed:

- [ ] Existing components and dependencies are reused per [Reuse Protocol](02-implementation.md#reuse-protocol) and [Contribution & Duplication Rules](02-implementation.md#contribution--duplication-rules).
- [ ] Ownership, RAII, moves, and view lifetimes follow [Memory & Resource Management](02-implementation.md#memory--resource-management) and [String Handling](02-implementation.md#string-handling).
- [ ] Interfaces and dependency injection follow [Const Correctness](02-implementation.md#const-correctness) and [Function & Constructor Signatures](02-implementation.md#function--constructor-signatures).
- [ ] Error contracts, exception safety, `noexcept`, and catch boundaries follow [Error Handling](03-error-handling.md), particularly [Exception Safety & noexcept](03-error-handling.md#exception-safety--noexcept) and [Exception Discipline & Catch Block Rules](03-error-handling.md#exception-discipline--catch-block-rules).
- [ ] External validation, bounds, and side-effect-free assertions follow [Security](#security) and [Contracts & Assertions](03-error-handling.md#contracts--assertions).
- [ ] Shared-state synchronization and documented thread-safety follow [Concurrency](#concurrency).
- [ ] Logging respects [Layered Logging Policy](#layered-logging-policy), CLI output separation, and sensitive-data restrictions.
- [ ] Relevant tests cover changed behavior and failure paths; results and platform-validation gaps are recorded, and performance claims have measurements.
- [ ] Relevant owner documentation and its language counterparts are synchronized per [Documentation Maintenance](#documentation-maintenance).
