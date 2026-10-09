# Language, Compiler & Build

**Requirement levels:** **must / must not** are hard requirements; **prefer / normally** describe defaults; **may** permits an exception under the stated conditions. These are contributor requirements, not claims of automated enforcement. Commands, tool activation conditions, and current CI coverage live in [Development](../docs/development.md).

## Language and build

- **Must** build in C++23 mode with CMake 3.28+ on a 64-bit target, using
  GCC 14+, Clang 19+, or Apple Clang 15+.
- **Prefer** standard C++23 facilities where they improve the implementation.
  Use C++20-compatible alternatives when a required C++23 facility is unavailable
  in a supported compiler or standard library; do not lower the build's language
  standard.
- **Must** keep build changes compatible with supported toolchains. Use the
  existing CMake modules rather than introducing a parallel build system.

## Dependencies

- **Must** use the established dependency bootstrap for bundled dependencies,
  declare an explicit release tag or full Git commit SHA, and avoid floating
  branches or moving aliases such as `main`, `latest`, or `v1`. The bootstrap
  in use is recorded in [Dependency maintenance](../docs/development.md#dependency-maintenance).
- **Prefer** upstream versioned release tags when available; use a full commit
  SHA when no suitable release exists or immutable source identity is needed.
  Release tags can be moved upstream: they are version selections, not immutable
  pins. A full commit SHA identifies immutable source content.
- **Must** keep the CPM bootstrap script in version control and review dependency
  additions and upgrades for compatibility, licensing, and security advisories.
- **Prefer** a small dependency set and periodic vulnerability reviews; do not
  assume dependency retrieval provides security auditing.

## Build quality and tooling

- **Must** fix warnings in changed first-party code rather than broadly disabling
  diagnostics. Any necessary suppression must be narrow and justified.
- **Must** follow the repository's `.clang-format` style and investigate relevant
  static-analysis diagnostics. Tool configuration alone does not establish a CI
  gate, nor does static analysis replace review of naming or object lifetimes.
- **Prefer** sanitizer-enabled Debug builds for development and a separate
  instrumented build for deeper testing. Available checks depend on the toolchain;
  do not assume a universal full sanitizer combination.
- **Must not** enable AddressSanitizer in production Release builds. Keep sanitizer
  testing separate from production performance measurements.
