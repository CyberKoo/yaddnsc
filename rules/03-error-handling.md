# Error Handling

> **Core Principles (authoritative)** — The rules below are the single source of truth.
> Subsequent subsections only clarify or provide lookup tables; they do not restate these.

## Core Principles

- Use `std::expected<T, E>` for errors where the caller can meaningfully continue (retry, fallback to another backend, etc.).
- Use **exceptions** only when the current call chain must terminate immediately and the caller cannot meaningfully continue — the exception is a terminate signal, not a value-passing mechanism.
- Mark functions that **truly cannot fail** as `noexcept`.
  - **Boundary rule**: A function that returns `std::expected` but internally performs heap allocation
    (e.g., constructing `std::string`/`std::vector`, inserting into a map) must **NOT** be marked
    `noexcept`. An OOM there would invoke `std::terminate`, defeating the graceful-failure intent of
    `expected`. Mark `noexcept` only when the function does no allocation and makes no fallible system call.
- Destructors must never throw (always implicitly or explicitly `noexcept`).
- Avoid C-style error codes (`int` return, `errno`).
- Guarantee at least **basic exception safety** (invariants preserved, no leaks).
  Strive for **strong exception safety** (commit-or-rollback) using **Copy-and-Swap**:
  ```cpp
  class Resource {
  public:
      void swap(Resource& other) noexcept { /* member-wise swap */ }
      Resource& operator=(const Resource& other) {
          Resource temp(other); // may throw
          swap(temp);           // never throws
          return *this;
      }
  };
  ```

## Function Design: Expected vs. Throwing

The fundamental rule is: **functions that can fail in ways the caller should handle must return `std::expected`. Functions that only fail when the program cannot proceed may throw.**

The root cause of misuse is almost always in the **function's design**, not in how callers handle it. A function that throws for retryable I/O errors is incorrectly designed; wrapping it in try-catch at the call site papers over the problem rather than fixing it.

```cpp
// BAD: Function throws for expected/retryable errors
void send(const Packet& p);  // throws on timeout, checksum error — caller might want to retry

// GOOD: Function returns expected for recoverable errors
[[nodiscard]] std::expected<void, SendError> send(const Packet& p) noexcept;
```

(See `Core Principles` for the `noexcept` allocation boundary note above.)

## Exception Discipline & Catch Block Rules

**Do NOT use exceptions as flow control.** An exception means the current operation has failed and cannot proceed. Functions that encounter errors the caller could meaningfully handle must use `std::expected`, not throw.

**`catch` blocks are permitted only for:**
1. **Logging** — record the error and (optionally) rethrow or terminate. Only rethrow when this is not the final termination point.
2. **Error translation at module boundaries** — convert an exception into a `std::expected` error value (`catch → return std::unexpected(...)`). Static mapping of exception type → error code is allowed; it does not select recovery strategies.
3. **Resource cleanup / rollback in exception-safe code** — restore invariants when an operation fails partway through (see strong exception safety example in `Core Principles`).
4. **Logging at the final termination point** (no rethrow).

**A `catch` block MUST NOT contain business flow control logic:**
- No retry loops.
- No fallback to alternative backends.
- No conditional branching that selects among different recovery strategies.

### Deep Call Chain Boundary Translation (permitted pattern)

When internal functions form a deep chain where converting every level to `std::expected` adds excessive mechanical propagation with no logic gain, internal functions may throw (for legitimate terminate conditions), and a single catch at the module's public API boundary performs error translation. This pattern must satisfy **all** of:
1. The catch performs **only error translation** — no retry, no fallback, no flow control.
2. The catch site is the **module's public API boundary**, not an internal function.
3. Internal functions throw only for **legitimate terminate conditions**, not for caller-handled errors.
4. The mechanical overhead of per-level `std::expected` propagation is **genuinely excessive** relative to logic gain.

If internal error types diverge or become recoverable, convert the internals to native `std::expected`.

## Decision Table

| Scenario | Mechanism | Rationale |
|----------|-----------|-----------|
| Caller can retry the same or fall back to another backend | `std::expected<T, E>` | Error is a value; the caller decides what to do next. |
| Caller cannot proceed (precondition violated, resource unavailable) | `throw` | The operation is aborted; no recovery at this layer. |
| Module boundary: internal throw + public `std::expected` API | Internal `throw` + outermost `catch` for error translation | Public contract uses `expected`, but internal fatal errors use exceptions. Catch only at the boundary, only for translation. |
