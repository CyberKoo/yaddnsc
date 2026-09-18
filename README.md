# yaddnsc — Yet Another Dynamic DNS Client

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![CI](https://github.com/CyberKoo/yaddnsc/actions/workflows/ci.yml/badge.svg)](https://github.com/CyberKoo/yaddnsc)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![codecov](https://codecov.io/github/CyberKoo/yaddnsc/graph/badge.svg?token=OA6OJQ3MN6)](https://codecov.io/github/CyberKoo/yaddnsc)
![Linux](https://img.shields.io/badge/Linux-glibc%20%7C%20musl-FCC624?logo=linux&logoColor=black)
![macOS](https://img.shields.io/badge/macOS-arm64-000000?logo=apple)
![FreeBSD](https://img.shields.io/badge/FreeBSD-supported-AB2B28?logo=freebsd)

> **Status:** `master` is the release branch (currently `v1.0.0-alpha.2`).
> `dev` contains changes after that release, and `v0.x` is maintained for
> legacy toolchains. v1 is pre-release software; rebuild driver plugins when
> upgrading between builds or changing toolchains.

**yaddnsc** monitors local or externally discovered IP addresses and updates DNS
records when they change. It supports multiple domains, IPv4/IPv6 records,
several DNS resolver protocols, and 12 bundled provider drivers.

## Table of Contents

- [Features](#features)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Usage](#usage)
- [Configuration](#configuration)
- [IP Sources](#ip-sources)
- [DNS Resolver](#dns-resolver)
- [TLS and CA Certificates](#tls-and-ca-certificates)
- [Production Deployment](#production-deployment)
- [Troubleshooting](#troubleshooting)
- [Developer Documentation](#developer-documentation)
- [License](#license)

## Features

- Manage multiple domains and subdomains from one JSON configuration.
- Configure independent A and AAAA records and update intervals.
- Obtain addresses from a local interface, an HTTP(S) endpoint, or mDNS.
- Update records through bundled drivers for 12 DNS providers.
- Resolve records with traditional DNS, DNS-over-HTTPS, or DNS-over-TLS.
- Choose concurrent, fallback, or shuffled DNS resolver strategies.
- Stop cleanly on SIGINT/SIGTERM and cancel in-flight network operations.
- Run on Linux (glibc/musl), macOS, and FreeBSD.

Provider-specific parameters and credential requirements are documented in
[DRIVERS.md](DRIVERS.md). 中文用户请参阅 [DRIVERS_CN.md](DRIVERS_CN.md)。

## Installation

Pre-built packages are not published yet. You can build from source, create a
Debian package, or use the supplied Dockerfile.

### Build from source

You need CMake 3.28+, OpenSSL 3.0+, and a C++23 compiler: GCC 14+, Clang
19+, or Apple Clang 15+.

Debian/Ubuntu prerequisites:

```bash
sudo apt install build-essential cmake pkg-config libssl-dev
```

macOS prerequisites:

```bash
brew install cmake pkg-config openssl@3
```

Build and install:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo cmake --install build
```

The default install places the binary under the selected install prefix,
drivers under `${libdir}/yaddnsc/drivers`, and the system configuration under
`${sysconfdir}/yaddnsc/config.json` (normally `/etc/yaddnsc/config.json`).
Use `-DCMAKE_INSTALL_PREFIX=...` at configure time for a different prefix.

### Debian package

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DYADDNSC_ENABLE_DEB=ON
cmake --build build --parallel
cpack --config build/CPackConfig.cmake -G DEB
```

A Docker-based builder is also available:

```bash
./docker/build-deb.sh
```

### Docker

```bash
docker build -t yaddnsc .
docker run --rm yaddnsc --help
```

## Quick Start

Create `config.json`, validate it, then run the client:

```bash
yaddnsc config test
yaddnsc run
```

The following is a **configuration-shape example**. The `simple` URL is a
placeholder and must be replaced with an API endpoint that actually performs
the DNS update; it will not update a real record as written.

```json
{
  "driver": { "auto_discover": true },
  "domains": [
    {
      "name": "example.com",
      "update_interval": 300,
      "driver": "simple",
      "subdomains": [
        {
          "name": "home",
          "type": "a",
          "ip_source": "http",
          "ip_source_param": "https://api.ipify.org",
          "driver_param": {
            "url": "https://your-api.example.com/update?ip={ip_addr}"
          }
        }
      ]
    }
  ]
}
```

For a real provider, choose a driver from [DRIVERS.md](DRIVERS.md) and copy its
`driver_param` example. Run `config test` before starting the update loop.

## Usage

```bash
# Run with ./config.json
yaddnsc run

# Run with a specific file and debug logging
yaddnsc run -c /etc/yaddnsc/config.json -d

# Validate configuration; -q/--quiet suppresses the success message
yaddnsc config test -q

# Print configuration, drivers, interfaces, or resolver information
yaddnsc config show
yaddnsc driver list
yaddnsc driver info <name>
yaddnsc interface list
yaddnsc interface ip <name>
yaddnsc dns resolver

# Resolve a hostname
yaddnsc dns resolve <hostname> --type A

yaddnsc info
yaddnsc --version
yaddnsc --help
```

Commands that fail return a non-zero exit status. A DNS lookup that cannot
resolve a record is reported by the diagnostic command and does not itself
start the update loop.

### Shell completions

Completion files for bash, zsh, and fish are installed with the package or
`cmake --install`. Restart the shell after installation if completion is not
available.

## Configuration

yaddnsc reads `./config.json` by default. The `-c` option belongs to the leaf
command, so put it after the command you are invoking. For example:

```bash
yaddnsc run -c /etc/yaddnsc/config.json
yaddnsc config test -c /etc/yaddnsc/config.json -q
yaddnsc config show -c /etc/yaddnsc/config.json
```

`yaddnsc config -c ... test` is not valid because `config` is only a command
group; `-c` is defined by `config test` and `config show`.

### Minimal structure

```json
{
  "driver": {
    "driver_dir": "/opt/yaddnsc/drivers",
    "load": ["cloudflare.so"]
  },
  "resolver": {
    "use_custom_server": true,
    "servers": [{ "address": "1.1.1.1", "port": 53 }]
  },
  "domains": [
    {
      "name": "example.com",
      "update_interval": 300,
      "driver": "cloudflare",
      "subdomains": [
        {
          "name": "home",
          "type": "a",
          "ip_source": "interface",
          "interface": "eth0",
          "driver_param": {
            "zone_id": "replace-me",
            "record_id": "replace-me",
            "token": "replace-me"
          }
        }
      ]
    }
  ]
}
```

### General fields

| Object | Field | Description |
|---|---|---|
| `driver` | `driver_dir` | Directory containing driver libraries. Omitted uses the installed driver directory. |
| `driver` | `auto_discover` | Load every `.so` in `driver_dir`; when true, `load` is ignored. |
| `driver` | `load` | Driver library names to load manually. |
| `resolver` | `use_custom_server` | Use configured servers instead of the built-in default resolver. When `true`, at least one server is required (see [DNS Resolver](#dns-resolver)). |
| `resolver` | `servers` | DNS server list; see [DNS Resolver](#dns-resolver). Takes precedence over the legacy `address`/`port` pair. |
| `resolver` | `strategy` | `concurrent`, `fallback`, or `shuffle`. |
| `domains[]` | `name` | Managed domain, such as `example.com`. |
| `domains[]` | `update_interval` | Default update interval in seconds; must meet the configured minimum. |
| `domains[]` | `force_update` | Periodic forced update interval; `0` disables it. |
| `domains[]` | `driver` | Name of a loaded driver. |
| `domains[]` | `subdomains` | Records managed under this domain. |
| `subdomains[]` | `name` | Label, or `@` for the apex record. |
| `subdomains[]` | `type` | Record type. Use `a` or `aaaa` for DDNS updates. `txt` is available to `dns resolve`, but update support is driver-specific and currently documented only for Cloudflare. |
| `subdomains[]` | `ip_source` | `interface`, `http`, or `mdns`. |
| `subdomains[]` | `ip_source_param` | URL for `http`, hostname for `mdns`; unused for `interface`. |
| `subdomains[]` | `interface` | Interface name. Required by `interface` source and optional for network sources. |
| `subdomains[]` | `update_interval` | Per-record interval; omitted or `0` inherits the domain interval. |
| `subdomains[]` | `driver_param` | Provider-specific settings; see [DRIVERS.md](DRIVERS.md). |

`allow_ula` and `allow_local_link` control whether IPv6 interface addresses in
those address ranges are accepted. Deprecated compatibility fields may still be
accepted; use the current fields above for new configurations.

Keep credentials out of source control and restrict the configuration file:

```bash
chmod 600 /etc/yaddnsc/config.json
```

Use the minimum provider permissions necessary for DNS updates. `config show`
redacts sensitive `driver_param` fields by default: any member whose key
(lower-cased) contains `token`, `password`, `secret` or `key` is printed as
`"***"`. The configuration file itself still holds the real values — keep
restricting it as shown above, and check logs before sharing them.

## IP Sources

### `interface`

Reads an address from a local network interface. A records use IPv4 and AAAA
records use IPv6. The interface must exist and be usable by the service.

### `http`

Fetches an address from an HTTP(S) endpoint whose response body is a plain IP
address. HTTPS is recommended. The optional `interface` controls the outgoing
network interface, subject to operating-system routing and permissions.

### `mdns`

Queries a local `.local` hostname using multicast DNS, for example
`printer.local`. It is intended for LAN devices, not public DNS names. mDNS may
not work in containers, VPNs, cloud hosts, or networks that block multicast;
IPv6 mDNS may require an explicit interface.

## DNS Resolver

The resolver uses configured servers when `use_custom_server` is enabled;
otherwise it uses the built-in default resolver, whose server is fixed at
build time — normally `1.1.1.1:53`. Maintainers can change it with
`-DYADDNSC_DEFAULT_DNS_SERVER=...` and `-DYADDNSC_DEFAULT_DNS_PORT=...` when
configuring the build.

Enabling `use_custom_server` requires at least one server, provided either
through the `servers` array or the legacy `address`/`port` pair. An empty
custom resolver is an invalid configuration: both `run` and `config test`
fail validation with "use_custom_server is enabled but no custom resolver
servers are configured", before any driver is loaded.

Normalisation rules: `servers` takes precedence over the legacy `address`
field; when `servers` is empty, the legacy `address` and `port` are folded
into the server list. When `use_custom_server` is disabled, the legacy fields
are ignored and the built-in default server is used.

- **Traditional DNS:** use an IP address and `port`; UDP is used with TCP
  fallback for large responses.
- **DoH:** use a complete `https://host/path` address, such as
  `https://1.1.1.1/dns-query`. The port is read from the URI and defaults to
  `443`.
- **DoT:** use a `tls://host[:port]` address. The port is read from the URI and
  defaults to `853`.

For DoH and DoT, the `port` field in the server object is ignored. The query
strategy is:

| Strategy | Behaviour |
|---|---|
| `concurrent` | Query resolver backends in parallel and use the first successful result. |
| `fallback` | Try configured resolvers in order. |
| `shuffle` | Try resolvers sequentially in a randomized order. |

## TLS and CA Certificates

TLS certificate verification is enabled by default. To use a private CA bundle,
set `SSL_CERT_FILE` before running yaddnsc:

```bash
export SSL_CERT_FILE=/etc/ssl/private/company-ca-bundle.pem
yaddnsc config test
yaddnsc run
```

The bundle is selected from `SSL_CERT_FILE`, the OpenSSL default path, or a
platform system path, and is cached for the process lifetime. If no trust store
is available, verification remains enabled and TLS connections fail safely.
`SSL_CERT_DIR` is not supported; use a combined PEM bundle instead.

## Production Deployment

After installation, verify the configuration before enabling the service. A
systemd unit is installed only when systemd development metadata is available
during a source install; Debian packages always include it.

```bash
yaddnsc config test
sudo systemctl daemon-reload
sudo systemctl enable --now yaddnsc
sudo systemctl status yaddnsc
journalctl -u yaddnsc
```

The service validates the configuration before starting. The installed system
configuration is normally `/etc/yaddnsc/config.json`. If the package provides
an environment override, use the installed service documentation and keep the
configuration readable only by the service account.

## Troubleshooting

| Symptom | First checks |
|---|---|
| Driver not found | Run `yaddnsc driver list`; check `driver_dir` and installation. |
| Configuration rejected | Run `yaddnsc config test`; check driver name, record type, and intervals. |
| DNS lookup fails | Run `yaddnsc dns resolver`; check server address, port, and firewall. |
| HTTP source fails | Check that the endpoint returns only an IP address and that HTTPS/route access works. |
| TLS verification fails | Check system time, CA bundle, and `SSL_CERT_FILE`. |
| mDNS returns no answer | Check the `.local` name, multicast support, interface, container network, and firewall. |
| systemd fails | Run `systemctl status yaddnsc` and `journalctl -u yaddnsc`. |
| ABI mismatch | Rebuild the driver against the current SDK — the host requires an exact `api_revision` match. |

## Developer Documentation

- [DNS provider drivers](DRIVERS.md)
- [Custom drivers and ABI compatibility](docs/custom-drivers.md)
- [Development, testing, and coverage](docs/development.md)
- [Architecture notes](docs/architecture.md)
- [Documentation map](docs/README.md)

## License

This project is licensed under the terms specified in the [LICENSE](LICENSE)
file.
