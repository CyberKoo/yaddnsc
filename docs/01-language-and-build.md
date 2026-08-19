# Language, Compiler & Build

## Language Standard

- Use **C++23** features wherever applicable. Fall back to **C++20** only when
  a required C++23 feature is not yet available in the supported compilers
  (GCC 14+, Clang 19+, AppleClang 15+).

## Compiler & Build

- Target compilers: **GCC 14+**, **Clang 19+**, **Apple Clang 15+**.
- Use **CMake** as the build system, minimum version **3.28**.
- Use **CPM.cmake** (v0.40+) for dependency management. Declare dependencies using `CPMAddPackage` with an explicit, immutable pin:
  - Prefer a **version tag** (e.g., `@2.6.2`) when the upstream project publishes version releases.
  - Fall back to a **full Git commit hash** (`GIT_TAG` with the complete SHA) only when the project does not publish version tags.
  - Do **not** use floating branches or mutable tags (e.g., `main`, `latest`, `v1`).
  Example:
  ```cmake
  CPMAddPackage("gh:CLIUtils/CLI11@2.6.2")
  CPMAddPackage("gh:Neargye/magic_enum@0.9.8")
  ```
- Document the rationale for using CPM in the project build documentation. Be aware of CPM's limitations (no binary caching, no transitive dependency resolution, no centralized security advisory registry) and mitigate them by keeping the dependency set small and performing periodic manual vulnerability reviews.
- Commit the dependency script to version control and review any dependency additions or version changes in pull requests.

### Compiler Warnings

- Base strict warning set for **all** builds (GCC/Clang): `-Wall -Wextra -Wpedantic -Wshadow -Werror`.
- **Conversion warning gate** (`-Wconversion -Wsign-conversion`): these conflict heavily with the standard library and common idioms, so they are **not** forced on every local build. They run as a **dedicated CI job** (e.g., a `ConversionGate` build) to keep developer velocity while still catching narrowing bugs before merge.

### Sanitizers

- **Default Debug builds**: enable `-fsanitize=address,undefined` for fast, low-false-positive coverage during development. Set:
  ```bash
  export ASAN_OPTIONS=detect_stack_use_after_return=1:strict_string_checks=1:detect_invalid_pointer_pairs=2
  ```
- **Full sanitizer combo** (`address,undefined,integer,bounds,null,alignment` plus `-fsanitize-address-use-after-return=always` and `-fsanitize-address-use-after-scope`) is extremely expensive and triggers many false positives against STL internals. It is **not** forced on developer debug builds. Run it only as a **dedicated CI sanitizer job** (e.g., `-DCMAKE_BUILD_TYPE=Sanitizer`) for periodic deep testing.
- **Do not use Address Sanitizer in Release builds** due to significant performance overhead (2–5× slowdown, increased memory usage).

## Tooling

- **clang-format**: Enforces code style. Configuration:
  ```yaml
  BasedOnStyle: Chromium
  ColumnLimit: 120
  IncludeBlocks: Regroup
  SortIncludes: CaseSensitive
  ```
- **clang-tidy**: Enforces naming conventions, modern C++ usage, and bug-prone patterns (see `.clang-tidy`). Must enable these use-after-move/free checks for **all build types** (static analysis, zero runtime overhead):
  - `bugprone-use-after-move`
  - `clang-analyzer-cplusplus.Move`
  - `clang-analyzer-cplusplus.InnerPointer`
