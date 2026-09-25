# yaddnsc — Yet Another Dynamic DNS Client

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![CI](https://github.com/CyberKoo/yaddnsc/actions/workflows/ci.yml/badge.svg)](https://github.com/CyberKoo/yaddnsc)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![codecov](https://codecov.io/github/CyberKoo/yaddnsc/graph/badge.svg?token=OA6OJQ3MN6)](https://codecov.io/github/CyberKoo/yaddnsc)
![Linux](https://img.shields.io/badge/Linux-glibc%20%7C%20musl-FCC624?logo=linux&logoColor=black)
![macOS](https://img.shields.io/badge/macOS-arm64-000000?logo=apple)

> **Status:** the current release is `v1.0.0-alpha.2` (pre-release). `master`
> tracks releases, `dev` accumulates changes intended for the next one, and
> `v0.x` remains available for older toolchains. The driver plugin ABI may
> change between builds; recompile externally built drivers after upgrading.

yaddnsc is a dynamic DNS client. For every configured record it periodically
acquires an IP address — from a local network interface, an HTTP(S) endpoint,
or multicast DNS — and compares it with the address currently published in
DNS. When the two differ, it submits an update through a provider-specific
driver. A single JSON file manages any number of domains, and each record can
carry its own type, address source, and update interval.

## Contents

- [Features](#features)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Command-Line Usage](#command-line-usage)
- [Configuration](#configuration)
- [IP Address Sources](#ip-address-sources)
- [DNS Resolver](#dns-resolver)
- [TLS and CA Certificates](#tls-and-ca-certificates)
- [Running as a Service](#running-as-a-service)
- [Troubleshooting](#troubleshooting)
- [Further Documentation](#further-documentation)
- [License](#license)

## Features

- Multiple domains and records managed from one JSON configuration.
- Independent record type (A/AAAA), address source, and update interval per
  record, with an optional periodic forced update.
- Address acquisition from a local interface, an HTTP(S) endpoint, or mDNS.
- Twelve provider drivers shipped as loadable modules; additional drivers can
  be built against the installed SDK.
- DNS resolution over UDP/TCP, DNS-over-HTTPS, or DNS-over-TLS, with
  concurrent, fallback, and shuffle query strategies.
- Configuration and driver parameter validation before the update loop starts.
- Graceful shutdown on SIGINT/SIGTERM with cancellation of in-flight requests.
- Linux (glibc and musl) and macOS (arm64).

Driver parameters and credential requirements are documented in
[DRIVERS.md](DRIVERS.md) (Chinese version: [DRIVERS_CN.md](DRIVERS_CN.md)).

## Installation

No pre-built packages are published at this time. The options are building
from source, producing a Debian package, or building a container image.

### Build from Source

Requirements:

- CMake 3.28 or later
- OpenSSL 3.0 or later
- A C++23 compiler: GCC 14+, Clang 19+, or Apple Clang 15+
- libxml2 (optional; without it the `namecheap` and `route53` drivers are not
  built)

All remaining dependencies are retrieved by the build system.

Debian/Ubuntu:

```bash
sudo apt install build-essential cmake pkg-config libssl-dev
```

macOS:

```bash
brew install cmake pkg-config openssl@3
```

Configure, build, and install:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo cmake --install build
```

When `CMAKE_BUILD_TYPE` is not specified the build defaults to Debug; pass
`Release` for production use. The install tree places the executable in the
`bin` directory of the prefix, driver modules in its library directory
(`<prefix>/lib/yaddnsc/drivers` on a default install), and a sample
configuration under `<sysconfdir>/yaddnsc/config.json`. Use
`-DCMAKE_INSTALL_PREFIX=...` to select a different prefix.

### Debian Package

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DYADDNSC_ENABLE_DEB=ON
cmake --build build --parallel
cpack --config build/CPackConfig.cmake -G DEB
```

Alternatively, `docker/build-deb.sh` builds the package inside an Ubuntu
container (24.04 by default) and writes the result to `deb-out/`; consult the
script header for supported options.

### Container Image

The Dockerfile in the repository root produces an Alpine-based runtime image:

```bash
docker build -t yaddnsc .
docker run -d --name yaddnsc \
  -v /etc/yaddnsc/config.json:/etc/yaddnsc/config.json:ro \
  yaddnsc
```

The image entrypoint is the `yaddnsc` executable; the default command is
`run -c /etc/yaddnsc/config.json`.

## Quick Start

Create a `config.json`, validate it, and start the client:

```bash
yaddnsc config test
yaddnsc run
```

A minimal configuration using the Cloudflare driver:

```json
{
  "driver": { "auto_discover": true },
  "domains": [
    {
      "name": "example.com",
      "update_interval": 300,
      "driver": "cloudflare",
      "subdomains": [
        {
          "name": "home",
          "type": "a",
          "ip_source": "http",
          "ip_source_param": "https://api.ipify.org",
          "driver_param": {
            "zone_id": "your-zone-id",
            "record_id": "your-record-id",
            "token": "your-api-token"
          }
        }
      ]
    }
  ]
}
```

Except for Route 53, every bundled driver updates an existing record; create
the record with the provider first, then copy the identifiers it assigns
(`zone_id`, `record_id`, and similar) into `driver_param`. The parameters of
each driver are listed in [DRIVERS.md](DRIVERS.md).

## Command-Line Usage

```bash
# Start the update loop (./config.json is the default configuration)
yaddnsc run

# Specify a configuration file and enable debug logging
yaddnsc run -c /etc/yaddnsc/config.json -d

# Validate a configuration; -q suppresses the success message
yaddnsc config test -q

# Inspect configuration, drivers, interfaces, and resolver settings
yaddnsc config show
yaddnsc driver list
yaddnsc driver info <name>
yaddnsc interface list
yaddnsc interface ip <name>
yaddnsc dns resolver

# Perform a one-off DNS lookup
yaddnsc dns resolve <hostname> --type A

# Print build information or version
yaddnsc info
yaddnsc --version
```

The `-c/--config` option is defined on the individual commands (`run`,
`config test`, `config show`, `driver list`, `driver info`, `dns resolve`,
`dns resolver`) and therefore follows the command name. The `interface`
commands and `info` do not read a configuration file.

Commands exit with a non-zero status on failure. One exception applies:
`dns resolve` reports a failed lookup in its output but still exits with
status 0; only usage errors produce a non-zero exit status.

Completion files for bash, zsh, and fish are installed by the package and by
`cmake --install`. If completion is unavailable afterwards, start a new shell
session.

## Configuration

yaddnsc reads `./config.json` unless `-c` selects another file:

```bash
yaddnsc run -c /etc/yaddnsc/config.json
yaddnsc config test -c /etc/yaddnsc/config.json -q
```

Because `-c` belongs to the leaf commands, `yaddnsc config -c ... test` is
rejected; `config` is only a command group.

### Structure

```json
{
  "driver": {
    "driver_dir": "/opt/yaddnsc/drivers",
    "load": ["cloudflare.so"]
  },
  "resolver": {
    "use_custom_server": true,
    "strategy": "concurrent",
    "servers": [
      { "address": "1.1.1.1", "port": 53 },
      { "address": "https://1.1.1.1/dns-query" }
    ]
  },
  "domains": [
    {
      "name": "example.com",
      "update_interval": 300,
      "force_update": 86400,
      "driver": "cloudflare",
      "subdomains": [
        {
          "name": "home",
          "type": "a",
          "ip_source": "interface",
          "interface": "eth0",
          "driver_param": {
            "zone_id": "your-zone-id",
            "record_id": "your-record-id",
            "token": "your-api-token"
          }
        }
      ]
    }
  ]
}
```

### Field Reference

| Object | Field | Description |
|---|---|---|
| `driver` | `driver_dir` | Directory searched for driver modules. Defaults to the installed driver directory. |
| `driver` | `auto_discover` | Load every module found in `driver_dir`. When enabled, the `load` list is ignored. |
| `driver` | `load` | Explicit list of modules to load, for example `["cloudflare.so"]`. A module that cannot be loaded is a fatal error. |
| `resolver` | `use_custom_server` | Use the servers below instead of the built-in default. See [DNS Resolver](#dns-resolver). |
| `resolver` | `servers` | List of DNS servers. Takes precedence over the legacy `address`/`port` pair. |
| `resolver` | `strategy` | `concurrent` (default), `fallback`, or `shuffle`. |
| `domains[]` | `name` | Managed domain, for example `example.com`. |
| `domains[]` | `update_interval` | Update interval in seconds. The default lower bound is 60, adjustable at build time. |
| `domains[]` | `force_update` | Interval in seconds at which an update is pushed unconditionally, without comparing against DNS. `0` (default) disables it; otherwise the value must not be smaller than `update_interval`. |
| `domains[]` | `driver` | Name of the driver module handling this domain. |
| `domains[]` | `subdomains` | Records managed under this domain; at least one is required. |
| `subdomains[]` | `name` | Record label; `@` denotes the apex of the domain. |
| `subdomains[]` | `type` | `a` or `aaaa`; when omitted, `a` is assumed. `txt` is accepted but no bundled driver updates TXT records. |
| `subdomains[]` | `ip_source` | `interface`, `http`, or `mdns`. See [IP Address Sources](#ip-address-sources). |
| `subdomains[]` | `ip_source_param` | URL for the `http` source, host name for the `mdns` source; unused by `interface`. |
| `subdomains[]` | `interface` | Interface name. Required by the `interface` source; optional for the network-based sources. |
| `subdomains[]` | `update_interval` | Per-record interval in seconds. `0` (default) inherits the domain interval. |
| `subdomains[]` | `allow_ula` | Accept IPv6 unique-local addresses (fc00::/7) from an interface source. Default `false`. |
| `subdomains[]` | `allow_local_link` | Accept IPv6 link-local addresses (fe80::/10) from an interface source. Default `false`. |
| `subdomains[]` | `driver_param` | Driver-specific parameters. Valid only at subdomain level; see [DRIVERS.md](DRIVERS.md). |

### Credentials

`driver_param` commonly contains API tokens or keys. Keep the configuration
file out of version control, restrict its permissions, and grant each token
only the permissions the update requires:

```bash
chmod 600 /etc/yaddnsc/config.json
```

As a safeguard against accidental disclosure, `config show` masks values in
`driver_param` whose key contains `token`, `password`, `secret`, or `key`
(case-insensitive), printing them as `"***"`. The file on disk continues to
hold the real values.

## IP Address Sources

### `interface`

Reads an address from a local network interface named by the `interface`
field, which is mandatory for this source. A records receive an IPv4 address
and AAAA records an IPv6 address. Unique-local and link-local IPv6 addresses
are excluded unless `allow_ula` or `allow_local_link` is set.

### `http`

Issues a request to the URL in `ip_source_param` and parses the response body
as a bare IP address. HTTPS endpoints are recommended. The optional
`interface` field binds the outgoing request to a specific interface, subject
to system routing and permissions.

### `mdns`

Resolves a multicast DNS name given in `ip_source_param`, which must end in
`.local`. Only `a` and `aaaa` records are supported. This source is intended
for devices on the local network; it depends on multicast traffic and is
therefore generally unavailable in containers, across VPN tunnels, and on
cloud networks. The optional `interface` field selects the interface used for
the query and may be required for IPv6.

## DNS Resolver

Without custom configuration, the resolver queries a single built-in server,
`1.1.1.1:53` unless changed at build time with
`-DYADDNSC_DEFAULT_DNS_SERVER=...` and `-DYADDNSC_DEFAULT_DNS_PORT=...`.

Setting `use_custom_server` to `true` replaces the built-in server with the
configured list. At least one server must then be provided, either through
`servers` or through the legacy `address`/`port` pair; an empty custom
resolver fails validation in both `run` and `config test`. When `servers` is
present it takes precedence over the legacy pair, and when
`use_custom_server` is `false` all custom fields are ignored.

The `address` of a server entry selects the protocol:

| Form | Protocol | Port |
|---|---|---|
| Bare IP or host name, plus `port` | DNS over UDP, with TCP fallback | `port` (default 53) |
| `https://host/path` | DNS-over-HTTPS | Taken from the URI, default 443 |
| `tls://host[:port]` | DNS-over-TLS | Taken from the URI, default 853 |

For the URI forms the `port` field of the entry is ignored.

With several servers configured, `strategy` controls the query order:

| Strategy | Behaviour |
|---|---|
| `concurrent` | Queries resolvers in parallel and adopts the first successful answer. |
| `fallback` | Queries resolvers sequentially in the configured order. |
| `shuffle` | Queries resolvers sequentially in an order randomized per query. |

## TLS and CA Certificates

Server certificate verification is always enabled. The CA bundle is located
by consulting, in order: the `SSL_CERT_FILE` environment variable, the
OpenSSL default location, and a set of platform-specific system paths. If no
bundle is found, TLS connections fail rather than fall back to an unverified
connection. `SSL_CERT_DIR` is not consulted; combine additional certificates
into a single PEM bundle and point `SSL_CERT_FILE` at it:

```bash
export SSL_CERT_FILE=/etc/ssl/private/company-ca-bundle.pem
yaddnsc run
```

## Running as a Service

A systemd unit is installed when the systemd development files are present at
install time; the Debian package always includes it. The unit validates the
configuration before starting, restarts on failure, logs to the journal, and
runs with a dynamically allocated user under restricted privileges.

```bash
yaddnsc config test -c /etc/yaddnsc/config.json
sudo systemctl daemon-reload
sudo systemctl enable --now yaddnsc
sudo systemctl status yaddnsc
journalctl -u yaddnsc
```

The unit reads `/etc/yaddnsc/config.json`. To use a different path, set
`YADDNSC_CONFIG` in the optional environment file
`/etc/yaddnsc/default/yaddnsc`. Because the service runs under a dynamic user,
the configuration file must be readable by the service while remaining
inaccessible to unrelated accounts.

## Troubleshooting

| Symptom | Suggested checks |
|---|---|
| Driver not found | Run `yaddnsc driver list`; verify `driver_dir` and that the module was installed. |
| Configuration rejected | Run `yaddnsc config test` for the specific validation error. |
| DNS lookups fail | Run `yaddnsc dns resolver`; verify server address, port, and firewall rules. |
| HTTP source fails | Confirm the endpoint returns a bare IP address and is reachable over HTTPS. |
| TLS verification fails | Check system time, the CA bundle, and `SSL_CERT_FILE`. |
| mDNS yields no answer | Verify the `.local` name, multicast availability, and the selected interface. |
| Service fails to start | Inspect `systemctl status yaddnsc` and `journalctl -u yaddnsc`. |
| Driver rejected after upgrade | Rebuild the driver against the current SDK; the host requires an exact ABI revision match. |

## Further Documentation

- [DNS provider drivers](DRIVERS.md)
- [Custom drivers and ABI compatibility](docs/custom-drivers.md)
- [Development, testing, and coverage](docs/development.md)
- [Architecture notes](docs/architecture.md)
- [Documentation map](docs/README.md)

## License

This project is distributed under the MIT License; see [LICENSE](LICENSE).
