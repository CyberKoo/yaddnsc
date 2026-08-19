# Quality & Process

## Concurrency

- Protect shared data with `std::mutex`, `std::shared_mutex`, or lock-free atomics.
- Avoid global mutable state. Prefer dependency injection. If a thread-safe singleton is unavoidable, use the Meyers `call_once`/`constinit` pattern — never a bare mutable global variable.
- Document thread-safety guarantees for all public interfaces.

## Security

- Validate and sanitize all external inputs at system boundaries.
- Use `std::span`, `std::string_view`, and bounds-checked containers to prevent buffer overflows.
- Use brace initialization `{}` to prevent narrowing conversions.
- Avoid undefined behavior: defer to the C++ standard, not observed platform behavior.
- Consider integrating static analysis tools (Coverity, SonarQube) into CI for additional assurance.

## Testing

- Use **GoogleTest** (with **GoogleMock** as the sole mocking framework) as the testing framework. Do not introduce alternatives.
- Maintain minimum **80% line coverage** (track with `lcov`/`gcov` or equivalent); prioritize branch coverage on critical paths.
- Naming: `TEST(ClassName, MethodName_Scenario_ExpectedBehavior)`.
- Use mock objects for external dependencies; prefer virtual interface-based mocking.
- Write integration tests for component interactions and benchmarks for critical paths.
- Tests are part of the build; a failing test is a build failure in CI.

## Logging

*(Reuse requirement is stated in [Components That Must Be Reused](02-implementation.md#components-that-must-be-reused). This section covers only logging-specific conventions.)*

- Use the project's centralized logging facade exclusively. No `std::cout`, `printf`, or ad-hoc loggers.
- Production logs include timestamp, severity, source location, and message via spdlog.
- Do not log sensitive data (passwords, tokens, PII) unless explicitly scrubbed.

## Documentation

- Use Doxygen-style `/** ... */` or `///` comments for public API.
- Document preconditions, postconditions, and thread-safety guarantees.
- Add `// TODO(username): description` for planned improvements.

## Performance Optimization

- Write for clarity first. Optimize only after profiling identifies a bottleneck.
- Leverage move semantics and RVO/NRVO. Do not `std::move` on return values.
- Be aware of SSO in `std::string` and `std::function`.
- Use `reserve()` when final container size is known.
- Place micro-benchmarks in a dedicated suite; track historical performance in CI.

## Cross-Platform Development

- Reuse the project's platform abstraction layer. Isolate platform-specific code in designated implementation files, not public headers.
- Prefer the C++ standard library and `std::filesystem` over platform-specific APIs.
- Test on all target platforms in CI (maintain an explicit platform matrix) before merging.

## Version Control & Collaboration

- Follow **Semantic Versioning** (`MAJOR.MINOR.PATCH`).
- Use **`<Type>: <description>`** commit messages (`Fix:`, `Feat:`, `Docs:`, `Refactor:`, `Test:`, `Chore:`) with a capitalized imperative description, e.g. `Fix: Harden URI port parsing against malformed values`. No scope prefixes (e.g. `fix(core):` is not used in this repository).
- Prefer small, focused PRs (<400 lines). Link to issue tracker tickets.
- Squash-and-merge to mainline for a linear, bisectable history.

## Documentation Maintenance

- When adding, removing, or modifying features, APIs, dependencies, or build steps, **update all relevant `README*.md` files** to reflect the current state.
- If the project maintains multiple language versions of README (e.g., `README.md`, `README.zh-CN.md`), **all language versions must be updated simultaneously** and kept in sync. A discrepancy between language versions is considered a documentation bug.
- This includes (but is not limited to):
  - Build prerequisites and instructions
  - Dependency lists and versions
  - API usage examples
  - Configuration options and environment variables
  - Test and benchmark commands
- Review README accuracy and cross-language consistency as part of the [Code Review Checklist](#code-review-checklist).
- When adding, removing, or renaming a rules topic, update the numbered files under `docs/` and the index in [`.rules`](../.rules) in the same change.

## Code Review Checklist

Every code review must verify the following:
- [ ] No raw `new` or `delete` outside permitted contexts (ABI boundary factories / `dlopen` layers only).
- [ ] All resources managed via RAII.
- [ ] Exception safety guarantees are met.
- [ ] Thread-safety is documented and verified.
- [ ] No use-after-move or dangling references.
- [ ] Interfaces follow const-correctness.
- [ ] Input validation is performed on external data.
- [ ] Existing project infrastructure components are reused where applicable; no duplicate implementations of logging, error handling, configuration, threading, or string utilities.
- [ ] New third-party dependencies do not duplicate functionality already provided by existing dependencies.
- [ ] Unit tests cover new/modified functionality and pass.
- [ ] No C-style `(void)` casts are used; use `[[maybe_unused]]` instead. Do not suppress return values of non-`[[nodiscard]]` functions.
- [ ] `catch` blocks contain **no business flow control logic** (no retry, no fallback, no branching on error type for recovery strategy selection). Error translation (exception type → error code) is permitted at module boundaries.
- [ ] If a `catch` block performs error translation (`return std::unexpected(...)`), verify it is at a module boundary and the catch performs no logic beyond translation.
- [ ] Functions returning `expected` that may allocate are **not** marked `noexcept`.
- [ ] Relevant README.md files are updated and accurate.
- [ ] All language versions of README*.md are updated and in sync.
