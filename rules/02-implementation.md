# Implementation

**Requirement levels:** **must / must not** are hard requirements; **prefer / normally** describe defaults; **may** permits an exception under the stated conditions. Examples are explanatory, not additional rules.

## Scope & Architecture Boundaries

These rules apply to first-party C++ code. C ABI declarations and adapters **must** preserve their language, layout, and ownership contracts rather than expose C++ types. See [Architecture](../docs/architecture.md#layers) and [Custom Drivers](../docs/custom-drivers.md) for details.

| Scope | Required boundary |
|-------|-------------------|
| Domain | Pure business rules and value types; no I/O, threading, or third-party dependencies. `std::chrono` time/duration value types are permitted; the layer does not read the system clock. |
| Application | External capabilities accessed through ports; concrete infrastructure assembled in the composition root. |
| Infrastructure / adapters | System and third-party APIs adapted to internal contracts; platform-specific code isolated here. |
| Public SDK / plugins | Only public SDK/util headers; no host-internal dependencies or C++ objects, STL containers, or exceptions crossing the plugin C ABI. |

Application code **may** use a coroutine runtime's public task, group, waiting,
time-value and cancellation APIs. It **must not** reach implicit runtime
context, loop or clock implementation objects, coroutine frames or wait
registrations; runtime internals stay runtime-internal. Composition owns loop
creation and the run entry point. The boundary, and the headers it admits, are
described in [Architecture](../docs/architecture.md#coroutine-api-boundary).

The include-level parts of these boundaries are checked by the `architecture_guard` test, registered with CTest and run in CI. It is a textual include/pattern check, not a full analysis: passing it is necessary, not sufficient, for the boundaries above.

## Code Reuse & Component Selection

### Reuse Protocol

1. **Search** for existing functionality before implementing it.
2. **Prefer** extending a suitable existing component over adding a parallel implementation.
3. If reuse is unsuitable, **document** the reason in the change description.

### Components That Must Be Reused

| Category | Requirement |
|----------|-------------|
| Logging | Use the central logging system through the designated per-layer entry point (see [Logging entry points](../docs/architecture.md#logging-entry-points)); CLI output is separate. |
| Error / result types | Use `std::expected<T, E>` according to [Error Handling](03-error-handling.md); domain-specific error types and result aliases are allowed, duplicate result-wrapper abstractions are not. C ABI status values remain at the boundary. |
| Configuration | Use the established configuration subsystem; do not independently re-parse configuration or environment variables downstream. |
| Platform abstraction | Reuse OS adapters and detection utilities. New platform conditionals must be confined to designated adapters or necessary public ABI portability definitions, not scattered through business code. |
| Shared utilities | Reuse the existing utility surface rather than duplicate string/formatting implementations between the host and its plugins. The single implementation site is recorded in [Layers](../docs/architecture.md#layers). |
| Numeric representation | Use explicitly sized types where a protocol, ABI, or persistent format requires a fixed width. Use `std::size_t` for sizes/indices and suitable integer types for local arithmetic; validate narrowing and overflow. |

### Contribution & Duplication Rules

- Component changes **must** preserve documented contracts or update affected callers and tests together. **Prefer** backward-compatible extensions.
- New dependencies **must not** duplicate existing capabilities without a documented reason; review compatibility, licenses, and security implications.

## Headers & Include Management

- Headers **must** be self-contained and include what they use; do not rely on incidental transitive includes. **Prefer** forward declarations where they avoid unnecessary dependencies without compromising correctness.
- First-party production headers **must** use macro include guards derived from their path relative to the production source root: add the project's include-guard prefix, uppercase the path, replace path separators and the extension separator with underscores, and retain the extension letters. The same identifier **must** appear in `#ifndef`, `#define`, and the closing `#endif` comment. Test headers may follow their test-specific convention.
- Use `"..."` for host-internal/generated headers, `<yaddnsc/sdk/...>` and `<yaddnsc/util/...>` for public headers, and `<...>` for standard/third-party headers.
- A `.cpp` **normally** includes its own header first using the bare filename. Other host-internal includes **must** use paths relative to the source root, not `../` paths. Generated headers use their configured include-root-relative filenames.
- Include order and formatting **must** follow `.clang-format`; actual checks are described in [Include hygiene](../docs/development.md#include-hygiene).
- **Normally** use `.h` for declarations and small inline functions, `.hpp` for substantial template/header-only implementations, and `.cpp` for non-template implementations. Judge inline content by readability and compile-time dependencies, not a line-count threshold; do not rename existing files solely to enforce this preference.
- Headers **must not** contain namespace-scope `using namespace` directives. Function-local literal namespace imports **may** be used; **prefer** avoiding them in headers when practical.

## Memory & Resource Management

### Ownership & RAII

- Resources **must** have RAII owners, including memory, sockets, files, locks, and library handles. **Prefer** value semantics and the Rule of Zero.
- Use `std::unique_ptr` for exclusive heap ownership; use `std::shared_ptr` only when shared ownership is required. **Prefer** `make_unique` / `make_shared` where appropriate.
- Raw pointers and references **normally** borrow; ownership transfer at a C boundary **must** be explicit and documented. `weak_ptr` **may** express non-owning observation of shared ownership, including breaking cycles.
- Non-owning pointers, references, spans, and string views **must not** outlive their backing object. Return values **must not** refer to destroyed local storage; warnings are useful but do not prove lifetime safety.
- After moving, operations **must** satisfy the source type's documented moved-from contract. **Normally** reassign before reusing its value; do not assume a particular state unless guaranteed by that type.

### ABI Allocation Exceptions

- Raw `new` / `delete` **must not** be used in ordinary internal code. They **may** be used in ABI factories/destructors or low-level C adapters when required to implement an explicit ownership contract.
- Acquired resources **must** be placed under RAII immediately. Plugin instances **must** use the plugin's matching destroy entry point, not host-side `delete`; the library **must** remain loaded until its instances and callbacks are no longer used.
- Allocation and deallocation **must** remain on the sides prescribed by the ABI. A permitted raw factory allocation is not permission for unmanaged ownership elsewhere.

### Ownership in Signatures

| Intent | Typical parameter |
|--------|-------------------|
| Borrow an object | `T&` / `const T&`; pointer when absence is meaningful |
| Transfer exclusive ownership | `std::unique_ptr<T>` by value |
| Retain shared ownership | `std::shared_ptr<T>` by value |
| Inspect an existing shared handle | `const std::shared_ptr<T>&`; copying it acquires a separate ownership share |

**Prefer** borrowing the object directly when the function does not need to manipulate its ownership handle. A reference to a `shared_ptr` does not itself acquire ownership.

## Const Correctness

- Member functions **must** be `const` when they do not mutate observable state; `mutable` **may** support documented caching/synchronization.
- **Prefer** `const` / `constexpr` for immutable values and read-only interfaces appropriate to the type.
- `noexcept` **must** follow [Exception Safety & noexcept](03-error-handling.md#exception-safety--noexcept), not whether an operation can report failure.

## Coding Style & Formatting

Formatting **must** follow `.clang-format`; naming **must** follow this table. A tool configuration is not a claim that every convention has an automated gate.

| Element | Convention |
|---------|------------|
| Constants / enumerators | `UPPER_SNAKE_CASE` |
| Functions | `snake_case` |
| Classes / types | `PascalCase` |
| Class member variables | `snake_case_` |
| Passive struct fields | `snake_case` |

- Results whose omission is a bug **must** be `[[nodiscard]]`. A deliberate discard **normally** uses a named `[[maybe_unused]]` variable with a reason when non-obvious; do not add suppression for ordinary non-`nodiscard` calls. **Prefer** `[[maybe_unused]]` over C-style `(void)` casts for unused parameters/variables.
- **Prefer** ordinary return syntax unless trailing returns improve or enable the declaration.
- Integer conversions **must** preserve the intended range and signedness. Use explicit casts after establishing validity where needed; a cast alone is not a bounds check. Do not hide first-party problems with local warning-suppression pragmas.

## Classes & Structs

- **Normally** use `struct` for passive data and `class` for encapsulated invariants.
- **Prefer** the Rule of Zero. When custom destruction/copy/move behavior is needed, **review** all five special members and explicitly define or delete operations whose generated behavior would be incorrect. A custom destructor does not automatically require implementing every operation.
- Single-argument converting constructors **must** be `explicit` unless conversion is intended and documented.
- PIMPL **may** reduce dependencies in public C++ APIs; it does not make a C++ interface a stable plugin ABI. **Prefer** simple composition or constrained templates over CRTP unless CRTP solves a concrete need.

## Function & Constructor Signatures

- **Prefer** small, cohesive parameter lists. More than four constructor parameters is a review signal, not an automatic limit. Related dependencies **may** use an `XxxPorts` / `XxxDeps` / `XxxServices` reference bundle; avoid general service locators or unused bundled dependencies.
- Configuration **must** travel as cohesive domain slices or pre-built policy objects when forwarded across layers, rather than repeated unrelated strings/vectors. Adding a field should not require mechanically changing a long chain of signatures.
- Cross-cutting policies **must** be assembled in the composition root and injected; downstream code **must not** independently re-derive the same policy.
- A deadline **must** be part of the control flow around an operation rather than an argument passed down to it, so that cancellation reaches every await inside. Ports **must** be explicit dependencies, directly or through a cohesive bundle; global/singleton ports are prohibited. Where a foreign-function boundary exposes no cancellation concept, its deadline travels as a boundary argument — see [Concurrency & I/O model](../docs/architecture.md#concurrency--io-model).

## Enums

- C++ enums **must** use `enum class` unless an external interface requires otherwise. C ABI enums **must** follow the ABI declaration.
- **Prefer** `magic_enum` for appropriate reflection. Host enum formatting **must** use the existing shared fmt polyfill/formatter registration instead of independent formatter implementations; registered enums can be logged directly.
- **Prefer** `std::to_underlying` for explicit underlying conversions. Public SDK utilities **must not** acquire host-only formatting dependencies.

## Templates & Concepts

- **Prefer** concepts and `requires` clauses over new SFINAE/`enable_if` designs; use `if constexpr` for compile-time implementation branches.

## Standard Library Usage

- **Prefer** `span` for contiguous borrowed ranges, `string_view` for borrowed text, `optional` for genuine optional values, and `variant` for internal type-safe alternatives. Preserve required C ABI representations at boundaries.
- **Prefer** algorithms/ranges and range-based loops when clearer; explicit indexing is appropriate when the algorithm needs offsets or bounds checks.
- **Prefer** `chrono` for internal time/duration semantics; adapt to C/system representations at their boundaries.

## String Handling

- Read-only text **normally** uses `std::string_view`; use `std::string` when owning/storing text, and `const std::string&` when an existing owning string's NUL-terminated storage is specifically needed.
- Views **must** stay within backing-storage bounds and lifetime. `string_view::data()` does not guarantee NUL termination; `data()[size()]` **must not** be probed without an independent storage contract that guarantees readability there.
- A NUL-terminated C API **must** receive a known terminated string (e.g. an owning `std::string` via `c_str()`), or an explicitly documented C-string input. **Prefer** a length-aware API when available.
- Embedded NULs **must** be rejected when truncation would change the meaning of a path, hostname, or other externally validated value. Owning a string guarantees termination, not absence of embedded NULs.
- Repeated C calls **may** reuse one owned string instead of repeatedly copying a view. Returned C handles still require RAII.

## Lambda Expressions

- **Prefer** explicit captures and short, cohesive lambdas. Extract a named function when it improves readability, reuse, or testing; no fixed line limit applies.
- Recursive lambdas **may** use an explicit self parameter or named function object. Use `std::function` only when runtime type erasure is needed, not solely to enable recursion.
- Lambda `noexcept` declarations **must** follow the same exception guarantee as other functions.
