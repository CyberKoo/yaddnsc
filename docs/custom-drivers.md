# Custom Drivers

This document is for developers who want to add a DNS provider driver to
yaddnsc. End users configuring one of the bundled providers should start with
[`README.md`](../README.md) and [`DRIVERS.md`](../DRIVERS.md).

## Compatibility

Drivers are runtime-loaded shared libraries. Build a custom driver with the
same compiler family, compatible compiler version, C++ standard library, C++23
settings, and yaddnsc ABI as the host application. Rebuild the driver when the
host toolchain or ABI changes.

The host verifies a driver before use:

1. the driver magic value identifies a yaddnsc driver;
2. the compiler/build identity matches the host;
3. the driver ABI version is compatible after instantiation.

A failed check is a configuration/build error. Do not bypass it by weakening
verification; rebuild the driver with the host build configuration instead.

## Recommended build

Place the driver under `driver/<name>/` and rebuild the project:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The project driver build supplies the generated ABI and build-identity headers
and applies the same compiler settings as the main binary.

## Driver responsibilities

A driver normally:

- creates the provider request from the update context;
- validates the provider response;
- exposes provider metadata and the ABI version;
- uses `DEFINE_DRIVER_FACTORY(YourDriver)` in its implementation file.

Use the existing drivers as examples and keep provider-specific credentials in
`driver_param`. Do not put credentials in source code or log messages.

## Standalone shared library

Standalone builds are discouraged. If unavoidable, the driver must be built as
a `MODULE` library with position-independent code and must use the generated
headers and factory macro from a compatible yaddnsc source/build tree. A
successful compilation alone does not guarantee ABI compatibility.

## Troubleshooting

- `Driver not found`: check `driver_dir`, the file name, and installation.
- Magic or ABI mismatch: rebuild the driver with the same source/toolchain as
the host.
- Missing required parameters: compare `driver_param` with the provider entry
in [`DRIVERS.md`](../DRIVERS.md).

The public driver interface and generated ABI headers are the source of truth;
this guide intentionally avoids duplicating their full API documentation.
