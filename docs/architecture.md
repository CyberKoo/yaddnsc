# Architecture Notes

This is a maintainer-oriented overview. It complements the user guide rather
than defining its public configuration contract.

## Runtime flow

```text
JSON configuration
       |
       v
Driver loading + configuration validation
       |
       v
Scheduler / update tasks
       |
       +--> IP source: interface | HTTP(S) | mDNS
       |
       +--> DNS resolver: UDP/TCP | DoH | DoT
       |
       v
Provider driver -> provider HTTP API
```

`Manager` coordinates startup, driver loading, validation, scheduling, and
shutdown. `Updater` performs an individual record update. Driver instances are
shared by concurrent update tasks, so bundled drivers are stateless.

## Module boundaries

- `src/config/`: JSON data types, parsing, and validation.
- `src/core/`: lifecycle, scheduling, driver loading, and update orchestration.
- `src/ip_source/`: address discovery backends.
- `src/dns/`: protocol parsing and resolver implementations.
- `src/network/` and `src/http_client/`: cancellable socket, TLS, and HTTP
  transport.
- `driver/`: independently built provider modules.
- `include/`: public interfaces shared with driver modules.

## Compatibility boundaries

The driver interface is a C++ ABI boundary. The host validates module identity,
build identity, and ABI version at load time. See
[Custom Drivers](custom-drivers.md) before changing the interface or building a
module outside the project build.

## Documentation ownership

- User-facing behaviour: root `README.md` and `README_CN.md`.
- Provider parameters: `DRIVERS.md` and `DRIVERS_CN.md`.
- Build, tests, and CI: `development.md`.
- Driver interface and ABI: `custom-drivers.md`.

When changing a public command, configuration field, install location, or
provider parameter, update its owner document in the same change.
