# yaddnsc — Yet Another Dynamic DNS Client (legacy branch)

**yaddnsc** is a Dynamic DNS (DDNS) client that monitors your local IP addresses and automatically updates DNS records with supported providers when changes are detected. It is designed to be lightweight, modular, and extensible through a plugin-based driver system.

> **This is the `legacy` branch**, backported for older systems.
> - C++ standard lowered to **C++17** — compiles out of the box on **Ubuntu 20.04** with GCC 9 and OpenSSL 1.1.x (native packages, no PPA required).
> - **ABI-compatible** with **v0.x driver plugins** — existing `.so` drivers work without recompilation.
> - **Maintenance-only** — this branch will no longer receive new features; only critical bug fixes will be addressed. **For new features and better performance, switch to the `master` branch (v1.x).**
> - Resolver support for DoH (DNS-over-HTTPS) and DoT (DNS-over-TLS) has been backported from the master branch.

## Features

- **Multi-domain and multi-subdomain management** — manage multiple domains and subdomains from a single configuration file.
- **Pluggable driver architecture** — drivers are loaded as shared libraries (`.so`) at runtime via `dlopen`. Built-in drivers include:
  - [Cloudflare](https://www.cloudflare.com/) — updates DNS records via the Cloudflare API v4
  - [DigitalOcean](https://www.digitalocean.com/) — updates DNS records via the DigitalOcean API v2
  - [DNSPod](https://www.dnspod.com/) — updates DNS records via DNSPod API (supports both China and Global endpoints)
  - [Simple](https://github.com/CyberKoo/yaddnsc) — a generic HTTP driver with URL template substitution for custom API endpoints
- **Flexible IP source configuration** — each subdomain can choose between:
  - `interface` — obtain the IP from a local network interface
  - `url` — obtain the IP from an external HTTP service (e.g. `https://ifconfig.me`)
- **IPv4 and IPv6 support** — configure A and AAAA records independently.
- **Custom DNS resolver** — optionally use a specific DNS server instead of the system resolver. Supports **traditional DNS** (plain IP + port), **DNS-over-HTTPS (DoH)** (full HTTPS URL, e.g. `https://1.1.1.1/dns-query`), and **DNS-over-TLS (DoT)** (TLS URI with `tls://` scheme, e.g. `tls://1.1.1.1`). The protocol is auto-detected from the address prefix.
- **Forced update scheduling** — periodically force-update DNS records even when the IP hasn't changed.
- **Graceful shutdown** — handles SIGINT/SIGTERM via a dedicated signal-handling thread.
- **Thread-pool-based concurrency** — subdomain updates are dispatched to a thread pool for parallel execution.

## Prerequisites

| Tool / Library  | Minimum Version    |
|-----------------|--------------------|
| CMake           | 3.14               |
| C++ Compiler    | C++17 capable      |
| OpenSSL         | 1.1.x              |
| Zlib            | Any recent version |

### Distribution Compatibility

The table below lists the minimum distribution versions that satisfy the requirements above:

| Distribution         | Minimum Version       | Notes                                                                                |
|----------------------|-----------------------|--------------------------------------------------------------------------------------|
| Ubuntu               | 18.04 (Bionic)        | Needs pip-installed CMake (≥ 3.14). GCC 7 requires `stdc++fs` (handled automatically). |
| Debian               | 10 (Buster)           | Needs pip-installed CMake (system CMake 3.13). GCC 8 + OpenSSL 1.1.1.                 |
| RHEL / CentOS / Rocky / Alma | 8 | Needs `gcc-toolset-10` or newer (GCC 9+) from AppStream. CMake 3.20+. OpenSSL 1.1.1.<br>**RHEL 7** has OpenSSL 1.0.2 (incompatible — requires OpenSSL ≥ 1.1.1).<br>**RHEL 9** has OpenSSL 3.0 (compatible). |
| Fedora               | 30                   | GCC 9 + CMake 3.14 + OpenSSL 1.1.1.                                                 |
| openSUSE / SLES      | Leap 15.2 / SLES 15 SP2 | GCC 9 + CMake 3.16 + OpenSSL 1.1.1.<br>Leap ≤ 15.1 / SLES 15 SP1 have GCC 7 (incompatible). |
| Alpine Linux         | 3.11                 | GCC 9.2 + CMake 3.15 + OpenSSL 1.1.1d.<br>musl is auto-detected (LTO disabled automatically).<br>Alpine 3.10 (GCC 8.3) is theoretically possible but not recommended. |

## Getting the Source

Clone the repository and initialize all submodule dependencies:

```bash
# Option A: Clone with submodules in one step
git clone --recursive -b v0.x https://github.com/CyberKoo/yaddnsc.git

# Option B: Clone first, then initialize submodules separately
git clone -b v0.x https://github.com/CyberKoo/yaddnsc.git
cd yaddnsc
git submodule update --init --recursive --depth 1
```

> `--depth 1` performs a shallow clone, significantly reducing download size and disk usage.

If you already have the source and need to pull the latest submodule commits (e.g., after switching branches or pulling upstream changes):

```bash
git submodule update --recursive --depth 1
```

## Building

> **Ubuntu 20.04** (native packages work out of the box):

```bash
# Install system dependencies
sudo apt install libssl-dev zlib1g-dev build-essential cmake

# Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# The main binary will be at build/objs/yaddnsc
# Driver modules will be at build/objs/driver/*.so
```

> **Ubuntu 18.04** (system CMake 3.10 is too old — use a newer CMake via pip):

```bash
# Install system dependencies
sudo apt install build-essential libssl-dev zlib1g-dev python3-pip cmake git

# Install a newer CMake (3.14+) via pip
pip3 install --upgrade pip
pip install --user "cmake<3.24"

# Build
mkdir build && cd build
~/.local/bin/cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# The main binary will be at build/objs/yaddnsc
# Driver modules will be at build/objs/driver/*.so
```

> For other POSIX systems, ensure your compiler supports C++17 and your CMake version is 3.14 or higher.

### CMake Options

| Option                        | Default | Description                                                    |
|-------------------------------|---------|----------------------------------------------------------------|
| `CMAKE_BUILD_TYPE`            | Release | Set to `Debug` for debug builds                                |
| `LIBC_MUSL`                   | auto     | Enable musl-specific workarounds (auto-detected; manual override via `-DLIBC_MUSL=ON/OFF`) |
| `NO_RTTI`                     | OFF     | Disable RTTI for a smaller binary (`-fno-rtti`)                |

Third-party dependencies (spdlog, cpp-httplib v0.14.3, cxxopts, BS::thread_pool, fmt, nlohmann_json) are included as **git submodules**.

## Driver Plugin ABI Compatibility

Drivers built for **yaddnsc v0.x** are binary-compatible with this legacy branch. The driver ABI version (`DRV_VERSION`) remains unchanged at `"1000000"`. Drop your existing `.so` files into the driver directory and they will work without recompilation.

## Configuration

yaddnsc uses a JSON configuration file. By default it looks for `./config.json`, or you can specify a custom path with the `-c` flag.

### Example Configuration

```json
{
  "driver": {
    "driver_dir": "/opt/yaddnsc/drivers",
    "load": [
      "cloudflare.so",
      "simple.so"
    ]
  },
  "resolver": {
    "use_custom_server": false,
    "ipaddress": "1.1.1.1",
    "port": 53,
    "protocol": "system"
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
          "type": "aaaa",
          "interface": "eth0",
          "ip_type": "ipv6",
          "ip_source": "interface",
          "ip_source_param": "",
          "allow_ula": false,
          "allow_local_link": false,
          "driver_param": {
            "zone_id": "your-zone-id",
            "record_id": "your-record-id",
            "token": "your-api-token"
          }
        },
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

> **DoH example:** To use DNS-over-HTTPS, set `ipaddress` to a full HTTPS URL:
> ```json
> { "ipaddress": "https://1.1.1.1/dns-query" }
> ```
> DoH uses the port from the URI (443); the `port` field is ignored. The address must start with `https://` and include the complete path (typically `/dns-query`). The protocol is auto-detected from the prefix, so you can omit `"protocol": "doh"`.

> **DoT example:** To use DNS-over-TLS, use the `tls://` prefix:
> ```json
> { "ipaddress": "tls://1.1.1.1", "port": 853 }
> ```
> DoT uses the `port` field, which must be set to 853.

### Configuration Reference

#### Top-level

| Field      | Type     | Description                                   |
|------------|----------|-----------------------------------------------|
| `driver`   | object   | Driver loading configuration                  |
| `resolver` | object   | Custom DNS resolver settings (optional)       |
| `domains`  | array    | List of domain configurations                 |

#### `driver` object

| Field           | Type     | Description                                            |
|-----------------|----------|--------------------------------------------------------|
| `driver_dir`    | string   | Directory containing driver `.so` files (optional)     |
| `load`          | string[] | List of driver shared library filenames to load        |

#### `resolver` object

| Field               | Type    | Description                                                                                                 |
|---------------------|---------|-------------------------------------------------------------------------------------------------------------|
| `use_custom_server` | boolean | If true, use the specified DNS server instead of the system default                                         |
| `ipaddress`         | string  | DNS server address: plain IP (e.g. `1.1.1.1`), DoH HTTPS URL, or `tls://` URI for DoT                      |
| `port`              | integer | DNS server port, defaults to 53 (optional)                                                                  |
| `protocol`          | string  | **Deprecated.** Kept for compatibility. Protocol is auto-detected from the `ipaddress` prefix (`https://` → DoH, `tls://` → DoT); manual setting is unnecessary. |

#### `domains[]` object

| Field             | Type   | Description                                                              |
|-------------------|--------|--------------------------------------------------------------------------|
| `name`            | string | Domain name (e.g. `example.com`)                                         |
| `update_interval` | int    | Interval in seconds between updates (minimum: 60)                        |
| `force_update`    | int    | Interval in seconds for forced updates (0 = disabled)                    |
| `driver`          | string | Name of the driver to use (must match a loaded driver)                   |
| `subdomains`      | array  | List of subdomain records to manage                                      |

#### `subdomains[]` object

| Field              | Type    | Description                                                                                      |
|--------------------|---------|--------------------------------------------------------------------------------------------------|
| `name`             | string  | Subdomain name (e.g. `home` for `home.example.com`)                                              |
| `type`             | string  | DNS record type: `"a"`, `"aaaa"`, `"txt"`, or `"soa"`                                           |
| `interface`        | string  | Network interface name (e.g. `eth0`). **Always required**, even when `ip_source` is `"url"`      |
| `ip_type`          | string  | IP version: `"ipv4"`, `"ipv6"`, or `"unspecified"` (optional, default: `"unspecified"`)          |
| `ip_source`        | string  | IP source: `"interface"` (from local network interface) or `"url"` (from external HTTP service)  |
| `ip_source_param`  | string  | HTTP(S) URL when `ip_source` is `"url"` (e.g. `https://api.ipify.org/`)                          |
| `allow_ula`        | boolean | Allow unique local addresses (IPv6 only), defaults to `false` (optional)                         |
| `allow_local_link` | boolean | Allow link-local addresses (IPv6 only), defaults to `false` (optional)                           |
| `driver_param`     | object  | Driver-specific parameters (key-value pairs)                                                     |

> **Note:** `interface` is always required. When `ip_source` is `"url"`, it is still passed to the HTTP client for socket binding. Set it to `""` if interface binding is not needed.

## Writing Custom Drivers

This branch retains the same driver API as v0.x. See the built-in drivers in `driver/` for reference.

### Driver API

Drivers implement the `IDriver` interface (defined in `include/IDriver.h`):

```cpp
class IDriver {
public:
    virtual driver_request generate_request(const driver_config_type &config) const = 0;
    virtual bool check_response(std::string_view response_body) const = 0;
    virtual driver_detail get_detail() const = 0;
    virtual std::string_view get_driver_version() const = 0;
    virtual void init_logger(int level, std::string_view pattern) = 0;
};
```

- `generate_request(config)` — receives the `driver_param` map from the subdomain config and returns a `driver_request` struct (URL, body, content type, HTTP method, headers).
- `check_response(response)` — receives the raw HTTP response body string; returns `true` if the update was successful.
- `get_detail()` — returns driver metadata (name, description, author, version).
- `get_driver_version()` — returns the ABI version constant. `BaseDriver` provides a `final` implementation; do not override.

The `BaseDriver` class (in `driver/base_driver.h`) provides useful helpers:

| Method                    | Description                                                        |
|---------------------------|--------------------------------------------------------------------|
| `check_required_params()` | Validates that required keys exist in `driver_param`               |
| `get_optional()`          | Safely retrieves an optional parameter from `driver_param`         |
| `vformat(format, args)`   | Named (`std::map`) or positional (`std::vector`) string formatting |

### Driver Factory

Each driver must export a `create()` factory function with C linkage:

```cpp
// cloudflare.h
extern "C" inline IDriver *create() {
    return new CloudflareDriver;
}
```

Place this in the driver's header file. The core loads drivers via `dlopen` and looks up the `create()` symbol.

### Build as a shared library

Drivers are built as `MODULE` libraries (position-independent, no `lib` prefix):

```cmake
add_library(cloudflare MODULE cloudflare.cpp cloudflare.h)
target_link_libraries(cloudflare PRIVATE yaddnsc_lib)
```

## Upgrading to v1.x

> **Warning:** The `master` branch (v1.x) is under heavy development. The v1 ABI has not yet been finalized and may change significantly — plugins **must** be recompiled after each update.

The `master` branch (v1.x) is a complete rewrite with significant improvements:

- **C++23** with modern standard library features, better performance, and fewer external dependencies
- **mDNS IP source** — discover LAN device addresses via mDNS (RFC 6762), e.g. `printer.local`
- **Interactive CLI** — diagnostic subcommands for inspecting drivers, network interfaces, DNS resolution, and configuration at runtime
- **Overhauled build system** — install rules, DEB packaging, and Docker containerization support
- **ABI versioning with driver magic validation** — forward-compatible versioning, legacy v0.x `.so` drivers are **not** compatible
- **Resolver identity system** — stable numeric IDs for unambiguous log cross-referencing
- **Improved DoT robustness** — non-blocking connection with configurable timeout

Switch to `master` if your system can meet the build requirements (GCC 14+, Clang 18+, AppleClang 15+, CMake 3.28+).

## Dependencies

This branch uses **git submodules** for dependency management.

| Library                                                     | Purpose                 | Management  |
|-------------------------------------------------------------|-------------------------|-------------|
| [spdlog](https://github.com/gabime/spdlog)                  | Logging                 | submodule   |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) v0.14.3 | HTTP client (OpenSSL 1.1.x compatible) | submodule |
| [cxxopts](https://github.com/jarro2783/cxxopts)             | CLI option parsing      | submodule   |
| [BS::thread_pool](https://github.com/bshoshany/thread-pool) | Thread pool             | submodule   |
| [fmt](https://github.com/fmtlib/fmt)                        | String formatting       | submodule   |
| [nlohmann_json](https://github.com/nlohmann/json)           | JSON parsing            | submodule   |
| OpenSSL (1.1.x)                                             | TLS support             | system      |
| Zlib                                                        | Compression             | system      |

## License

This project is licensed under the terms specified in the [LICENSE](LICENSE) file.
