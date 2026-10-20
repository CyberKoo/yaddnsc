# Documentation

- [User guide](../README.md)
- [DNS provider drivers](../DRIVERS.md)
- [Custom drivers (authoring guide)](custom-drivers.md)
- [Plugin ABI contract](plugin-abi.md)
- [Architecture notes](architecture.md)
- [Runtime flow](runtime-flow.md)
- [Concurrency and I/O model](concurrency.md)
- [Coroutine runtime contracts](coroutine.md)
- [Development and build](development.md)
- [Testing and coverage](testing.md)
- [CI and static analysis](ci.md)
- [Changelog](../CHANGELOG.md)

## Documentation ownership

- User-facing behaviour: root `README.md` and `README_CN.md`.
- Provider parameters: `DRIVERS.md` and `DRIVERS_CN.md`.
- Driver authoring (build, SDK usage, responsibilities): `custom-drivers.md`.
- Driver ABI contract (versioning, layout, validation, lifecycle):
  `plugin-abi.md`.
- Architecture boundaries and plugin host wiring: `architecture.md`; the
  business update cycle: `runtime-flow.md`; the thread and I/O model:
  `concurrency.md`; the coroutine runtime contract: `coroutine.md`.
- Build configuration and the ABI-change process: `development.md`.
- Test, coverage, sanitizer and benchmark commands: `testing.md`.
- CI jobs and static analysis: `ci.md`.
- Release notes and behaviour changes: `../CHANGELOG.md`.

When changing a public command, configuration field, install location, or
provider parameter, update its owner document in the same change. State each
rule once, in its owner document, and link to it from elsewhere.
