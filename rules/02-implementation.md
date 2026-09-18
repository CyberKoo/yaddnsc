# Implementation

## Code Reuse & Component Selection

**Reuse existing, proven foundation components** whenever they satisfy requirements. Do not reinvent logging, error handling, configuration management, threading primitives, or platform abstraction layers.

### Reuse Protocol

1. **Search the existing codebase** for similar functionality before implementing anything new.
2. **Evaluate the existing component** against current requirements. If close but missing a capability, extend it in a backward-compatible manner.
3. **Document the decision** if you choose not to reuse an existing component, explaining why it was insufficient.

### Components That Must Be Reused

| Category | Requirement |
|----------|-------------|
| **Logging** | Use the project's centralized logging facade. No `std::cout`/`printf` or ad-hoc loggers. |
| **Error/Result types** | Use `std::expected<T, E>` consistently. Do not introduce per-module error wrappers. |
| **Configuration** | Use the established configuration subsystem. Do not parse env vars or config files independently. |
| **Platform abstraction** | Use existing platform detection macros and OS-abstraction utilities. No `#ifdef` blocks in new code. |
| **String utilities** | Use shared `trim`, `split`, `join`, case conversion from the project's string library. |
| **Numeric types** | Use fixed-width types (`std::uint8_t`, `std::int32_t`, etc.) at API boundaries, for serialization, and for persistent storage. Local transient counters/indices where overflow is impossible may use plain `int`. Use project-standard safe arithmetic wrappers where they exist. |

### Contribution & Duplication Rules

- Improve reused components in place; ensure all existing callers still compile and pass tests.
- Do not pull in third-party libraries that duplicate functionality of existing dependencies. Review the dependency graph before adding new libraries.

## Headers & Include Management

- Include order is enforced by clang-format (`IncludeBlocks: Regroup`).
- Include syntax:
  - Host-internal and generated headers (from `src/` and `generated/`) use `#include "..."`.
  - Public SDK headers use `#include <yaddnsc/sdk/...>`.
  - Shared utility headers (host + plugins) use `#include <yaddnsc/util/...>`.
  - Third-party library headers and standard library headers use `#include <...>`.
- Include path conventions:
  - A `.cpp` file should include its own `.h` header using the bare filename
    (e.g. `#include "dispatcher.h"` for `dispatcher.cpp`). This is automatically
    placed first by clang-format.
  - All other host-internal headers must use the full path relative to the
    `src/` base directory (e.g. `#include "infrastructure/dns/types.h"`,
    `#include "infrastructure/dns/dns_lookup_exception.h"`).
  - Do **NOT** use `../` relative paths to reach other modules.
  - Generated headers (from `generated/`): the `generated/` directory is added to the
    compiler's include root, so they are referenced by their flat filename
    (e.g. `#include "config_cmake.h"`). Do not use relative sub-paths to reach them.
    If a name collision ever occurs, rename the generator output; never work around it
    with a relative path.
- Minimize `#include` dependencies: forward-declare types where possible.
- Always include what you use (IWYU): do not rely on transitive includes.
  A symbol must come from a header you include directly — never from one
  pulled in incidentally by another header. This is enforced, not advisory:
  - IWYU runs as part of every Clang build (`cmake/IWYU.cmake`) and
    violations fail the build. Third-party noise is filtered through
    `.iwyu-mappings.imp`; when a suggestion is genuinely wrong, prefer a
    mapping entry over an in-source pragma.
  - Every first-party header must be self-contained (compile standalone).
    The `yaddnsc_header_checks` target (`cmake/HeaderCheck.cmake`) compiles
    each header as its own translation unit on every build. This is what
    keeps "works with libstdc++, fails with libc++" bugs off macOS.
  - clangd flags both problems in the editor (`.clangd` sets
    `UnusedIncludes`/`MissingIncludes` to `Strict`).
- File conventions:
  - **`.h`**: Interface declarations with limited inline implementations.
    Permitted inline content:
    - Pure getters (single `return` statement, `constexpr` or not).
    - Template methods ≤10 lines (excluding signature, blank lines, and lone brace lines).
    - Classification predicates (`is_*()`, `has_*()`) that are single expressions.
    - Factory methods that are single-expression returns (e.g. `from_array` delegating to `from_bytes`).
    - Type alias / tag / convenience wrappers (e.g. `using`, `visit()` forwarding to `std::visit`).
  - **`.hpp`**: Headers with template implementations exceeding the `.h` limits, non-trivial inline
    functions, or any detailed implementation logic that does not fit the `.h` criteria above.
  - **`.cpp`**: Non-template implementation files.
- Never place `using namespace` directives in header files at global or namespace scope.
  Function-body-local `using namespace std::string_literals;` is permitted in `.cpp` files
  and tolerated inside `.h`/`.hpp` function bodies, but should be avoided in headers when practical.

## Memory & Resource Management

### Allocation Rules (Memory)

- **Never** use raw `new` or `delete` outside permitted contexts.
- Use `std::unique_ptr` for exclusive ownership, `std::shared_ptr` for shared ownership.
- Prefer `std::make_unique` and `std::make_shared` for creating smart pointers.
- Raw pointers (`T*`) and `std::reference_wrapper<T>` are for non-owning observers only.
- Avoid `std::weak_ptr` unless needed for breaking cyclic dependencies.
- A moved-from object **must not be accessed** except for destruction or reassignment.

#### Exemptions for Raw `new`/`delete`

The prohibition does **not apply** to:
1. **Dynamic plugin/driver factory entry points** — C ABI-compatible factory functions (`extern "C"` linkage, invoked via `dlsym`) where C++ smart pointers cannot cross library boundaries safely due to ABI instability.
2. **External dynamic library loading code** — `dlopen`/`dlsym`/`dlclose` wrappers, C-style ABI interactions, and adapter layers bridging C APIs to C++ internals.

**Justification**: Dynamic plugin systems require ABI stability across compilers and standard library versions. Smart pointers do not guarantee stable ABIs. Raw `new`/`delete` usage is strictly confined to boundary-crossing factory functions. All internal code must use smart pointers.

**Ownership handoff**: Within an exemption, the factory may return a raw `T*` allocated with `new`. The internal caller that receives it must immediately wrap it in `std::unique_ptr<T>` (or equivalent RAII owner) at the call site; no other `.cpp` file may ever write a manual `delete`.

### RAII & Ownership Conventions (Resources)

- Follow **RAII** for **all** resources (file handles, locks, sockets, GPU resources, memory — see Allocation Rules above).
- Document ownership transfer in function signatures:
  - `std::unique_ptr<T>`: function takes ownership.
  - `const std::shared_ptr<T>&`: function shares ownership.
  - `T&` or `T*`: function does not take ownership.
- Returning a reference/pointer to a local object is a compile-time error (`-Wreturn-stack-address` + `-Werror`).

## Const Correctness

- Mark member functions `const` if they do not mutate observable state. Use `mutable` only for caching/synchronization.
- Prefer `const` references for read-only parameters and `const` iterators (`cbegin()`, `cend()`).
- Declare immutable variables `const` or `constexpr` by default.
- Mark functions that cannot fail as `noexcept` per the boundary rule in [Error Handling → Core Principles](03-error-handling.md#core-principles).

## Coding Style & Formatting

All formatting is enforced by **clang-format** (`BasedOnStyle: Chromium`). Naming conventions enforced by **clang-tidy**:

| Element | Convention |
|---------|------------|
| Constants | `UPPER_SNAKE_CASE` |
| Functions | `snake_case` |
| Classes | `PascalCase` |
| Class member variables | `snake_case_` (trailing underscore) |
| Pure data struct member variables (POD, no invariants) | `snake_case` (no trailing underscore) |

- Always add `[[nodiscard]]` where ignoring the return value is a bug.
- Use `[[maybe_unused]]` (C++17 attribute) instead of C-style `(void)` casts for deliberately discarding a return value. This applies to both local variables and function parameters.
- Do not suppress the return value of functions that are not `[[nodiscard]]` — the compiler will not warn, so the suppression is noise.
- Use trailing return type syntax only when necessary (e.g., dependent return types in templates).
- When crossing fixed-width integer types (e.g., `std::uint8_t` argument passed to an `int` parameter, or comparison with `std::size_t`), use an explicit `static_cast`. Do not silence with local `#pragma GCC diagnostic` blocks.

## Classes & Structs

- Use `struct` for passive data carriers (POD, public members, no invariants).
- Use `class` for types with invariants or private members.
- Follow the **Rule of 0** by default. If you define any of the five special member functions, explicitly define or delete all five.
- Mark single-argument constructors `explicit` unless implicit conversion is genuinely intended and documented.
- **Consider PIMPL** for stable public-facing APIs whose implementation changes frequently, to reduce compile-time dependencies. Note: a PIMPL class necessarily defines a destructor (and possibly move operations) for its incomplete-type `std::unique_ptr` member; this is a deliberate, localized exception to the Rule of 0 — disable copying unless explicitly required.
- Use **CRTP** sparingly; prefer concepts-based polymorphism.

## Enums

- Always use `enum class`. Avoid plain `enum`.
- Name enumerators in `UPPER_CASE_CONSTANT` style.
- Use **magic_enum** for reflection (name/enum conversion, iteration).
- Use the project's **fmt polyfill** for compile-time checked formatting of enum values. Do not use raw `std::format` or `fmt::format` directly. All enum `fmt::formatter` specializations are registered through the polyfill's shared, project-wide header; once registered there, **spdlog can log the enum directly** (`logger->info("state: {}", state)`). An enum not registered in the polyfill must not be logged.
- Use `std::to_underlying` (C++23) for underlying integer conversions when explicitly needed.
- Custom formatter specializations for enums go through the fmt polyfill's extension mechanism in a shared, project-wide header.

## Templates & Concepts

- Use **C++20 concepts** instead of SFINAE.
- Avoid `std::enable_if`; use `if constexpr` or concepts.
- Prefer `requires` clauses for template constraints.

## Standard Library Usage

- Prefer `std::span<T>` over `const std::vector<T>&` for read-only contiguous data.
- Use `std::string_view` for read-only string parameters (see [String Handling](#string-handling) for the full selection table and C-API safety rules).
- Prefer `std::optional<T>` over sentinel values (`-1`, `nullptr`).
- Use `std::variant<Ts...>` for type-safe unions instead of C unions or `void*`.
- Use range-based `for` loops and `<algorithm>`/`<ranges>` over explicit index-based loops.
- Use `std::chrono` for all time operations; avoid C-style time functions.

## String Handling

### Parameter Selection

| Use Case | Recommended Type | Rationale |
|----------|-----------------|-----------|
| Read-only operations | `std::string_view` | Zero overhead, flexible |
| One-time C API call | Accept `std::string_view`, copy internally | Convenient, cost acceptable |
| Frequent C API calls | `const std::string&` | Caller manages null-termination |
| Need ownership/mutation | `std::string` | Clear ownership semantics |
| C library interfaces | `const std::string&` | Guaranteed null-termination |

### Key Rules

- `string_view::data()` is **not guaranteed null-terminated**. Only use when the API explicitly accepts non-null-terminated buffers.
- When calling C functions requiring null-termination, explicitly convert to `std::string`.
- Avoid `const char*` except when interfacing with C libraries.
- Reuse existing string utility functions (`trim`, `split`, `join`, etc.).

### Performance Patterns

```cpp
// Read-only: zero overhead
[[nodiscard]] size_t count_words(std::string_view text) {
    return std::ranges::count(text, ' ') + 1;
}

// C library interaction: explicit cost
[[nodiscard]] bool open_file(const std::string& path) {
    return ::open(path.c_str(), O_RDONLY) >= 0;
}
[[nodiscard]] bool open_file(std::string_view path) {
    return open_file(std::string(path));  // Explicit copy
}

// Caching for repeated calls
class FileHandler {
    std::string path_;
public:
    explicit FileHandler(std::string_view path) : path_(path) {}
    void open() { ::open(path_.c_str(), O_RDONLY); }
};

// Lazy copy with null-termination check
[[nodiscard]] const char* get_c_str_or_copy(std::string_view sv, std::string& buffer) {
    if (!sv.empty() && sv.data()[sv.size()] == '\0') return sv.data();
    buffer = sv;
    return buffer.c_str();
}
```

## Lambda Expressions

- Prefer explicit captures over `[=]` or `[&]` defaults.
- Keep lambdas short (<10 lines). Extract longer ones into named functions.
- Mark lambdas `noexcept` when they cannot throw.
- Avoid recursive lambdas; use `std::function` or named function objects.
