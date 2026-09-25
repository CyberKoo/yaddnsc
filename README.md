# yaddnsc — Yet Another Dynamic DNS Client (v0.x, legacy)

> **Branch status.** This is the legacy branch, maintained for systems whose
> toolchains cannot build v1.x. The language requirement is lowered to C++17
> and OpenSSL 1.1.x is supported. The branch is maintenance-only: it receives
> bug fixes but no new features. Driver plugins built for v0.x remain
> binary-compatible with this branch; plugins built for v1.x are rejected.
> New development takes place on `master` (v1.x).

yaddnsc is a dynamic DNS client. For every configured record it periodically
acquires an IP address — from a local network interface or an HTTP(S)
endpoint — and compares it with the address currently published in DNS. When
the two differ, it submits an update through a provider-specific driver
loaded as a shared library. A single JSON file manages any number of domains.

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Getting the Source](#getting-the-source)
- [Building](#building)
- [Running](#running)
- [Configuration](#configuration)
- [IP Address Sources](#ip-address-sources)
- [DNS Resolver](#dns-resolver)
- [Drivers](#drivers)
- [TLS and CA Certificates](#tls-and-ca-certificates)
- [Running as a Service](#running-as-a-service)
- [Driver Plugin ABI](#driver-plugin-abi)
- [Writing Custom Drivers](#writing-custom-drivers)
- [Upgrading to v1.x](#upgrading-to-v1x)
- [Dependencies](#dependencies)
- [License](#license)

## Features

- Multiple domains and records managed from one JSON configuration, with a
  per-domain update interval and an optional periodic forced update.
- Address acquisition from a local network interface or an HTTP(S) endpoint.
- Provider drivers loaded as shared libraries at runtime; Cloudflare,
  DigitalOcean, DNSPod, and a generic HTTP driver are included.
- DNS resolution through the system resolver, a custom server over UDP,
  DNS-over-HTTPS, or DNS-over-TLS.
- A and AAAA records handled independently; TXT and SOA lookups are supported
  by the resolver.
- Concurrent record updates on a thread pool.
- Graceful shutdown on SIGINT/SIGTERM.

## Requirements

| Component | Minimum |
|---|---|
| CMake | 3.14 |
| C++ compiler | C++17 capable (GCC 9+ recommended; GCC 7/8 are supported through an automatic `stdc++fs` fallback) |
| OpenSSL | 1.1.1 or later (OpenSSL 3.x is also supported) |
| zlib | any recent version |

All other dependencies are vendored as git submodules; see
[Dependencies](#dependencies).

Reference points for common distributions:

| Distribution | Notes |
|---|---|
| Ubuntu 20.04 and later | Builds with native packages. |
| Ubuntu 18.04 | Requires a newer CMake (3.14+), for example via pip; the default GCC 7 relies on the automatic `stdc++fs` fallback. |
| Debian 10 and later | Requires a newer CMake (3.14+) than the system package provides. |
| RHEL / CentOS / Rocky / Alma 8 and later | Requires GCC 9 or newer (for example `gcc-toolset-10`); RHEL 7 ships OpenSSL 1.0.2 and is not supported. |
| Alpine 3.11 and later | musl is detected automatically and LTO is disabled accordingly. |

## Getting the Source

The dependencies are git submodules and must be fetched together with the
source:

```bash
# Clone with submodules
git clone --recursive -b v0.x https://github.com/CyberKoo/yaddnsc.git

# Or initialize them after the fact
git clone -b v0.x https://github.com/CyberKoo/yaddnsc.git
cd yaddnsc
git submodule update --init --recursive --depth 1
```

After switching branches or pulling, refresh the submodules:

```bash
git submodule update --recursive --depth 1
```

## Building

Ubuntu 20.04 and later:

```bash
sudo apt install build-essential cmake libssl-dev zlib1g-dev

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Ubuntu 18.04 (system CMake is too old):

```bash
sudo apt install build-essential libssl-dev zlib1g-dev python3-pip git
pip install --user "cmake<3.24"

mkdir build && cd build
~/.local/bin/cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The build produces the executable at `build/objs/yaddnsc` and the driver
modules at `build/objs/driver/*.so`. This branch has no install rules or
packages; copy the files into place manually (see
[Running as a Service](#running-as-a-service)).

### CMake Options

| Option | Default | Description |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | Set to `Debug` for a debug build. |
| `LIBC_MUSL` | auto-detected | musl-specific adjustments (disables LTO); override with `-DLIBC_MUSL=ON/OFF`. |
| `NO_RTTI` | `OFF` | Build with `-fno-rtti` for a smaller binary. |

## Running

```bash
yaddnsc -c /etc/yaddnsc/config.json
```

| Option | Description |
|---|---|
| `-c, --config <path>` | Configuration file path; default `./config.json`. |
| `-v, --verbose` | Enable debug-level logging. |
| `-V, --version` | Print the version and exit. |
| `-h, --help` | Print usage and exit. |

The configuration is loaded at startup and validated once the drivers have
been loaded; an invalid configuration aborts the start. On any fatal error
the program logs a critical message and exits with a non-zero status.

SIGINT and SIGTERM trigger a graceful shutdown: in-flight updates are
completed before the process exits. Repeated SIGINT escalates to an immediate
termination.

## Configuration

yaddnsc reads a JSON configuration file, `./config.json` by default. A
complete example is shipped as [`config.example.json`](config.example.json):

```json
{
  "driver": {
    "driver_dir": "/opt/yaddnsc/drivers",
    "load": ["cloudflare.so"]
  },
  "resolver": {
    "use_custom_server": false,
    "ipaddress": "1.1.1.1",
    "port": 53
  },
  "domains": [
    {
      "name": "example.com",
      "update_interval": 300,
      "force_update": 0,
      "driver": "cloudflare",
      "subdomains": [
        {
          "name": "home",
          "type": "a",
          "interface": "",
          "ip_type": "ipv4",
          "ip_source": "url",
          "ip_source_param": "https://api.ipify.org/",
          "allow_ula": false,
          "allow_local_link": false,
          "driver_param": {
            "sub_domain": "home.example.com",
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

### `driver`

| Field | Type | Description |
|---|---|---|
| `driver_dir` | string | Directory containing the driver modules; optional, default empty (paths in `load` are then relative to the working directory). |
| `load` | string[] | Driver module file names to load, including the `.so` suffix; required. Entries are resolved against `driver_dir`. |

### `resolver`

| Field | Type | Description |
|---|---|---|
| `use_custom_server` | boolean | Required. When `false`, the system resolver (`/etc/resolv.conf`) is used and the remaining fields carry no effect. |
| `ipaddress` | string | Required even when `use_custom_server` is `false`. Plain IP for classic DNS, an `https://` URL for DoH, or a `tls://` address for DoT; see [DNS Resolver](#dns-resolver). |
| `port` | integer | Server port; optional, default 53. |
| `protocol` | string | Ignored; retained for backward compatibility. The protocol is always derived from the `ipaddress` prefix. |

### `domains[]`

All fields are required.

| Field | Type | Description |
|---|---|---|
| `name` | string | Domain name, for example `example.com`. |
| `update_interval` | int | Update interval in seconds; minimum 60. |
| `force_update` | int | Interval in seconds at which an update is pushed unconditionally, without comparing against DNS. `0` disables it; otherwise the value must not be smaller than `update_interval`. |
| `driver` | string | Name of the driver handling this domain; must match a loaded driver. |
| `subdomains` | array | Records managed under this domain. |

### `subdomains[]`

| Field | Type | Description |
|---|---|---|
| `name` | string | Record label, for example `home` for `home.example.com`; required. |
| `type` | string | Record type: `a`, `aaaa`, `txt`, or `soa` (case-sensitive); required. Dynamic DNS updates use `a` or `aaaa`. |
| `interface` | string | Network interface name; the key is required but may be empty (`""`). When set, it selects the source interface and binds outgoing HTTP traffic; see [IP Address Sources](#ip-address-sources). |
| `ip_type` | string | `ipv4`, `ipv6`, or `unspecified` (default). Selects the socket family used for HTTP traffic — the `url` source request and the driver API request. |
| `ip_source` | string | `interface` or `url`; required. See [IP Address Sources](#ip-address-sources). |
| `ip_source_param` | string | The HTTP(S) URL to query when `ip_source` is `url`; unused by the `interface` source. The key is required. |
| `allow_ula` | boolean | Accept IPv6 unique-local addresses (fc00::/7) from an interface source; default `false`. |
| `allow_local_link` | boolean | Accept IPv6 link-local (fe80::/10) and site-local (fec0::/10) addresses from an interface source; default `false`. |
| `driver_param` | object | Driver-specific parameters; required. Keys and values must all be strings. |

Two fields with similar names serve different purposes: `type` determines the
record type and, for the `interface` source, the address family collected
locally; `ip_type` determines the address family of the HTTP connections made
for the `url` source and for driver API calls.

Before each update, the host adds the keys `domain`, `subdomain`, `ip_addr`,
`rd_type`, and `fqdn` to the driver's parameters with their runtime values.
Explicit entries in `driver_param` with the same names take precedence.
Drivers that declare these keys as required (for example `domain` for
DigitalOcean) therefore need no manual entry.

### Credentials

`driver_param` commonly contains API tokens or keys. Keep the configuration
file out of version control, restrict its permissions (`chmod 600
config.json`), and grant each credential only the permissions the update
requires.

## IP Address Sources

### `interface`

Reads an address from the local interface named by `interface`. A records
receive an IPv4 address and AAAA records an IPv6 address. Unique-local
addresses are excluded unless `allow_ula` is set; link-local and site-local
addresses are excluded unless `allow_local_link` is set.

### `url`

Issues an HTTP GET to `ip_source_param` and parses the response body as a
bare IP address. HTTPS endpoints are recommended. The `interface` field, when
non-empty, binds the outgoing connection to that interface; the same binding
applies to the driver's update request.

## DNS Resolver

With `use_custom_server: false`, lookups go through the system resolver. With
`use_custom_server: true`, the form of `ipaddress` selects the protocol:

| Form | Protocol | Port |
|---|---|---|
| Plain IPv4/IPv6 address | Classic DNS over UDP | `port` (default 53) |
| `https://host[/path]` | DNS-over-HTTPS | Taken from the URL (443); `port` is ignored |
| `tls://host` | DNS-over-TLS | `port` (default 53 — set it to 853 explicitly) |

Notes on each form:

- Classic custom servers are applied through the resolver library; on
  platforms without `res_nquery` the custom server is ignored and a warning
  is logged. IPv6 custom servers require platform support detected at build
  time.
- For DoH, an empty path defaults to `/dns-query`.
- For DoT, the `tls://` prefix is stripped and the remainder is used as the
  server host name (and the TLS SNI). The port is **not** inferred: it comes
  from `port`, whose default of 53 is unsuitable — set `"port": 853`.

A failed lookup is retried up to five times with a one-second interval before
the update cycle skips the record.

## Drivers

Four drivers are included, built as `cloudflare.so`, `digital_ocean.so`,
`dnspod.so`, and `simple.so`. A domain selects its driver by the name in the
`driver` field; parameters go into the record's `driver_param` as string
key-value pairs. A missing required key is reported when the affected record
is updated.

The keys `domain`, `subdomain`, `ip_addr`, `rd_type`, and `fqdn` are supplied
automatically at update time (see [Configuration](#configuration)) and are
omitted from the requirement lists below.

### Cloudflare (`cloudflare`)

Updates an existing record through the Cloudflare API v4 with a bearer API
token.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `sub_domain` | Yes | — | Full name of the record, for example `home.example.com` |
| `zone_id` | Yes | — | Zone ID of the domain |
| `record_id` | Yes | — | ID of the record to update |
| `token` | Yes | — | API token with DNS edit permission on the zone |
| `ttl` | No | `"30"` | TTL in seconds |
| `proxied` | No | `"0"` | Route the record through the Cloudflare proxy; truthy values are `1`, `on`, `true`, `yes` |

Note the spelling `sub_domain` (with underscore): it is not covered by the
automatically supplied `subdomain` key and must be given explicitly.

### DigitalOcean (`digital_ocean`)

Updates an existing record through the DigitalOcean API v2 with a personal
access token. Only the record content is modified; no TTL control exists.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `record_id` | Yes | — | ID of the record to update |
| `token` | Yes | — | Personal access token |

### DNSPod (`dnspod`)

Updates an existing record through the DNSPod API (`Record.Ddns`) with a
login token.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `domain_id` | Yes | — | Domain ID |
| `record_id` | Yes | — | Record ID |
| `login_token` | Yes | — | API token in `ID,Token` format |
| `global` | No | `"false"` | Truthy values (`1`, `on`, `true`, `yes`) select the international endpoint instead of the China endpoint |
| `record_line` | No | `"默认"` | Record line name |
| `record_line_id` | No | `"0"` | Record line ID |

### Simple (`simple`)

Issues an HTTP GET to a configured URL; intended for custom update endpoints.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `url` | Yes | — | The request URL |
| `format` | No | — | When this key is present (any value), the URL is treated as a template |

With `format` present, each placeholder `{name}` in the URL is replaced by
the value of the `driver_param` entry whose key is `{name}` (braces
included). For example, an entry `"{token}": "abc123"` makes `{token}`
available in the URL. Placeholders without a matching entry abort the update.
The automatically supplied runtime values (`ip_addr` and the others) are not
visible to the template on this branch; use this driver with endpoints that
derive the address from the request source, or upgrade to v1.x, where the
template can reference them.

The response is not inspected: any completed request counts as success. The
same applies to the Cloudflare driver on this branch.

## TLS and CA Certificates

HTTPS traffic — driver API calls, the `url` source, and the DoH/DoT
resolvers — is verified against a CA bundle located by searching, in order:
`./ca.pem` in the working directory, then a set of platform-specific system
paths (Debian/Ubuntu, RHEL/Fedora, openSUSE, macOS Homebrew, and others).

There is no `SSL_CERT_FILE` support on this branch. To use a private CA,
place a PEM bundle at `./ca.pem` next to the executable or install it into
the system location. If no bundle is found at all, certificate verification
is disabled and a notice is logged; ensure a bundle is present on production
systems.

## Running as a Service

A sample systemd unit is provided as [`yaddnsc.service`](yaddnsc.service). A
manual installation looks like:

```bash
sudo install -D build/objs/yaddnsc /opt/yaddnsc/yaddnsc
sudo install -D -t /opt/yaddnsc/drivers build/objs/driver/*.so
sudo install -D -m 600 config.json /etc/yaddnsc/config.json
sudo install -D yaddnsc.service /etc/systemd/system/yaddnsc.service

sudo systemctl daemon-reload
sudo systemctl enable --now yaddnsc
journalctl -u yaddnsc
```

The unit starts `/opt/yaddnsc/yaddnsc -c /etc/yaddnsc/config.json` as
`nobody` and restarts the process on failure. Adjust paths and the service
user to the installation; the configuration file must remain readable by the
service account.

## Driver Plugin ABI

Drivers are shared libraries loaded with `dlopen` at startup. Each driver
reports an ABI version that must match the host exactly; the version on this
branch is `1000000` and is unchanged across v0.x releases, so existing v0.x
`.so` files keep working without recompilation. Drivers built against the
v1.x SDK carry a different ABI and are refused at load time.

## Writing Custom Drivers

A driver implements the `IDriver` interface (`include/IDriver.h`) and is
built as a prefix-less `MODULE` library exporting two C symbols:

```cpp
extern "C" IDriver *create();          // factory
extern "C" void destroy(IDriver *);    // disposer
```

The interface consists of:

- `generate_request(config)` — receives the parameter map (including the
  automatically supplied runtime keys) and returns a `driver_request`
  describing the URL, method, headers, and body of the update request.
- `check_response(body)` — inspects the response body and reports whether the
  update succeeded.
- `get_detail()` — returns the driver name, description, author, and version;
  the name is what configurations reference in the `driver` field.
- `get_driver_version()` and `init_logger()` — provided by `BaseDriver`
  (`driver/base_driver.h`); do not override them.

`BaseDriver` additionally offers `check_required_params()` for required-key
validation (failing keys raise an exception at update time), `get_optional()`
for optional parameters, and `vformat()` helpers for named or positional
string substitution. The bundled drivers under `driver/` serve as complete
examples.

## Upgrading to v1.x

The `master` branch (v1.x) is a rewrite with a different driver ABI; v0.x
plugins cannot be used there and must be rebuilt against the new SDK. It
requires a current toolchain (CMake 3.28+, GCC 14+, Clang 19+, or Apple Clang
15+) and adds, among other things, an mDNS address source, a subcommand-based
CLI with configuration validation and diagnostics, resolver strategies over
multiple servers, install rules with DEB packaging, and a container image.
Configurations are not fully compatible between the branches; consult the
v1.x documentation when migrating.

## Dependencies

| Library | Version | Purpose |
|---|---|---|
| [spdlog](https://github.com/gabime/spdlog) | 1.13.0 | Logging |
| [fmt](https://github.com/fmtlib/fmt) | 10.2.1 | String formatting |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.14.3 | HTTP client |
| [nlohmann_json](https://github.com/nlohmann/json) | 3.11.3 | JSON parsing |
| [cxxopts](https://github.com/jarro2783/cxxopts) | 3.2.1 | Command-line parsing |
| [BS::thread_pool](https://github.com/bshoshany/thread-pool) | 4.1.0 | Thread pool |
| OpenSSL | system | TLS |
| zlib | system | Compression |

The libraries in the first six rows are git submodules under `deps/` and are
built as part of the project.

## License

This project is distributed under the MIT License; see [LICENSE](LICENSE).
