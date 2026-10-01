# Error Handling

**Requirement levels:** **must / must not** are hard requirements; **prefer / normally** describe defaults; **may** permits an exception under the stated conditions. These sections define the rules; [Examples](05-examples.md) are non-normative illustrations.

## Core Principles

- Failure contracts **must** distinguish internal programming defects from validated external-input failures and operational failures.
- Caller-handled operational failures **must** be explicit error values in project C++ APIs. Exceptions **may** abort the current operation under the boundary rules below; aborting an operation does not necessarily terminate the process.
- Error frequency is not the primary classification criterion. Choose by API contract and handling responsibilities; performance tradeoffs **must** be supported by measurement.

## Contracts & Assertions

- Preconditions constrain state before a call; postconditions constrain state after successful completion; invariants constrain valid object states. They are predicates about inputs, outputs, and state, not the inputs/outputs themselves.
- **Prefer** runtime assertions (`assert`) to diagnose internal contract and invariant violations caused by programming defects. Such violations **must not** be disguised as ordinary retryable business errors.
- Untrusted external inputs **must** be validated with runtime checks. Invalid configuration, malformed packets, and invalid ABI arguments **must** be rejected according to the interface contract, not treated as unchecked internal caller obligations.
- Assertion expressions **must not** have side effects. Disabling assertions **must not** remove required work, validation, or safety checks.
- Assertions may be disabled in Release builds. Where internal violations need production handling to preserve safety, the code **must** define an explicit failure policy rather than rely on a debug assertion alone.
- Public checked APIs **must** document failure behavior; their runtime validation is distinct from an unchecked internal precondition.

## Mechanism Selection

| Situation | Required / permitted mechanism |
|-----------|--------------------------------|
| Caller must inspect failure and decide how to continue, retry, skip, or fall back | `std::expected<T, E>`; recovery decisions remain in normal control flow. |
| External input fails validation | Runtime check followed by an error result or operation abort according to the API contract. |
| Internal contract / invariant violated | Assertion for diagnosis; explicit safe production failure policy where required. |
| Failure aborts the current operation with no recovery responsibility inside its call chain | Exceptions may propagate to the designated operation boundary. |
| Third-party or permitted deep internal implementation throws | Translate at the adapter/public module boundary into the documented error type. |
| Plugin C ABI entry point | Catch escaping C++ exceptions and convert to ABI status/error data; no exceptions may cross the ABI. |

- C++ result APIs **must** use `std::expected`, not new C-style status/`errno` conventions. C/system APIs and the plugin ABI **may** retain required status representations; adapters **must** preserve/capture their errors and translate as appropriate.
- Results that callers must inspect **must** be `[[nodiscard]]`. `expected` exposes failure in the type but does not force handling or prevent propagation. `.value()` on an error result throws `std::bad_expected_access`; **normally** branch on the result before accessing it.
- Returning `expected` is not a no-throw guarantee: allocation and other implementation operations can still throw.

## Exception Safety & noexcept

- `noexcept` **must** mean that no C++ exception can escape the function, not that the operation always succeeds. A function **may** report system-call failure through an error value and still be `noexcept` if the entire path is non-throwing.
- Functions with unhandled potentially throwing operations (including allocation) **must not** be marked `noexcept` merely because they return `expected`. An escaping exception would call `std::terminate`.
- A required no-throw boundary **must** contain exceptions and use a non-throwing failure-reporting path. Plugin ABI error reporting **must not** depend on successful allocation during exception handling.
- Destructors **must not** let exceptions escape; **normally** declare custom destructors `noexcept`. Resource release failures **must** follow a documented no-throw policy.
- Fallible operations **must** provide at least basic exception safety: preserve invariants and prevent resource leaks. **Prefer** strong commit-or-rollback semantics where feasible. Copy-and-swap is one option, not a mandatory implementation technique; rollback steps **must not** introduce another escaping exception.
- **Prefer** `noexcept` on functions whose implementation and contracts guarantee it, particularly moves, swaps, and resource release where applicable.

## Exception Discipline & Catch Block Rules

- `catch` **may** perform boundary error translation, final error presentation/logging, propagation logging, or necessary cleanup/rollback. **Prefer** RAII cleanup and logging once at the boundary that owns the diagnostic.
- `catch` **must not** select business recovery strategies, retry, or fall back to another backend. Translate the error first; normal caller control flow decides recovery.
- Translation **must** occur at a documented adapter, module API, operation, or ABI boundary, not arbitrary internal call sites. Mapping exception types to error categories is permitted; it is not a recovery strategy.
- Final operation boundaries **may** report failure without rethrowing; non-final boundaries **must** propagate or translate rather than silently turn failure into success.
- A deliberately best-effort, no-throw facility (e.g. SDK diagnostic logging) **may** discard its own failures under an explicit documented policy. This does not permit swallowing business-operation failures.

### Deep Call Chain Boundary Translation

Project-owned functions **normally** return `expected` directly for caller-handled failures. Internal throw-and-translate **may** be used when all of these hold:

1. The internal exception aborts the current module operation; no internal level selects a recovery strategy.
2. The public module API translates to its documented error value before business callers receive the result.
3. Per-level `expected` forwarding would add genuinely excessive mechanical propagation without handling logic.
4. The boundary performs translation and optional diagnostics only, not retry/fallback.

The DNS wire parser (`src/infrastructure/dns/parser.cpp`) is the established example: malformed packets abort parsing internally, and resolver boundaries expose an error value. The malformed packet is an external failure, not necessarily fatal to the process. **Prefer** direct `expected` for new shallow parsers such as `Uri::parse`; if intermediate levels need to handle failures, use native `expected` there.
