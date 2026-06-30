# yaddnsc — Yet Another Dynamic DNS Client (legacy branch)

**yaddnsc** is a Dynamic DNS (DDNS) client that monitors your local IP addresses and automatically updates DNS records on supported DNS providers when changes are detected. It is designed to be lightweight, modular, and extensible through a plugin-based driver system.

> **This is the `legacy` branch**, backported specifically for older systems.
> - C++ standard has been lowered to **C++17** — compiles out of the box on **Ubuntu 20.04** with GCC 9 and OpenSSL 1.1.x (native packages, no PPA required).
> - ABI compatible with **v0.x driver plugins** — your existing `.so` drivers will work without recompilation.
> - **Maintenance mode** — this branch will not receive new feature backports. It will only receive critical bug fixes. **New features and better performance are available on the `master` branch (v1.x).**
> - DoH (DNS-over-HTTPS) and DoT (DNS-over-TLS) resolver support has been backported from the master branch.

## Features

- **Multi-domain, multi-subdomain management** — manage multiple domains and subdomains from a single configuration file.
- **Pluggable driver architecture** — drivers are loaded as shared libraries (`.so`) at runtime via `dlopen`. Built-in drivers:
  - [Cloudflare](https://www.cloudflare.com/) — updates DNS records via the Cloudflare API v4
  - [DigitalOcean](https://www.digitalocean.com/) — updates DNS records via the DigitalOcean API v2
  - [DNSPod](https://www.dnspod.com/) — updates DNS records via DNSPod API (supports both China and Global endpoints)
  - [Simple](https://github.com/Kotarou/yaddnsc) — a generic HTTP driver with URL template substitution for custom API endpoints
- **Flexible IP source configuration** — per-subdomain, choose:
  - `interface` — obtain the IP from a local network interface
  - `url` — obtain the IP from an external HTTP service (e.g. `https://ifconfig.me`)
- **IPv4 and IPv6 support** — configure A and AAAA records independently.
- **Custom DNS resolver** — optionally use a specific DNS server instead of the system resolver. Supports **traditional DNS** (plain IP + port), **DNS-over-HTTPS (DoH)** (full HTTPS URL, e.g. `https://1.1.1.1/dns-query`), and **DNS-over-TLS (DoT)** (TLS URI with `tls://` schema, e.g. `tls://1.1.1.1`). Protocol is auto-detected from the address prefix.
- **Forced update scheduling** — periodically force-update DNS records even when the IP hasn't changed.
- **Graceful shutdown** — handles SIGINT/SIGTERM via a dedicated signal-handling thread.
- **Thread-pool based concurrency** — subdomain updates are dispatched to a thread pool for parallel execution.

## Build Requirements

### Prerequisites

| Tool / Library  | Minimum Version    |
|-----------------|--------------------|
| CMake           | 3.14               |
| C++ Compiler    | C++17 capable      |
| OpenSSL         | 1.1.x              |
| Zlib            | Any recent version |

yaddnsc is POSIX-only. Supported compilers: GCC 9+, Clang 10+

### Building

```bash
# Install system dependencies (Ubuntu 20.04)
sudo apt install libssl-dev zlib1g-dev build-essential cmake

# Clone with submodules
git clone --recursive -b v0.x https://github.com/Kotarou/yaddnsc.git
cd yaddnsc

# Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# The main binary will be at build/objs/yaddnsc
# Driver modules will be at build/objs/driver/*.so
```

### CMake Options

| Option                        | Default                                       | Description                                                       |
|-------------------------------|-----------------------------------------------|-------------------------------------------------------------------|
| `CMAKE_BUILD_TYPE`            | Release                                       | Set to `Debug` for debug builds                                   |
| `LIBC_MUSL`                   | OFF                                           | Enable musl-specific workarounds                                  |
| `NO_RTTI`                     | OFF                                           | Disable RTTI for a smaller binary (`-fno-rtti`)                   |

Third-party dependencies (spdlog, cpp-httplib v0.14.3, cxxopts, BS::thread_pool, fmt, nlohmann_json) are included as **git submodules** (not fetched via CPM).

### Driver Plugin ABI Compatibility

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
> { "ipaddress": "https://1.1.1.1/dns-query", "port": 443 }
> ```
> The address must start with `https://` and include the complete path (typically `/dns-query`). Protocol is auto-detected from the prefix, so you can omit `"protocol": "doh"`.

> **DoT example:** To use DNS-over-TLS, use `tls://` prefix:
> ```json
> { "ipaddress": "tls://1.1.1.1" }
> ```

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
| `use_custom_server` | boolean | If true, use the specified DNS server instead of system                                                     |
| `ipaddress`         | string  | DNS server address. Plain IP (e.g. `1.1.1.1`), HTTPS URL for DoH (e.g. `https://1.1.1.1/dns-query`), or `tls://` URI for DoT |
| `port`              | integer | DNS server port, default 53 (optional)                                                                      |
| `protocol`          | string  | DNS protocol: `"system"` (default), `"doh"`, or `"dot"` (optional, auto-detected from address if omitted)  |

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
| `allow_ula`        | boolean | Allow unique local addresses (IPv6 only), default `false` (optional)                             |
| `allow_local_link` | boolean | Allow link-local addresses (IPv6 only), default `false` (optional)                               |
| `driver_param`     | object  | Driver-specific parameters (key-value pairs)                                                     |

> **Note:** `interface` is always required, even when `ip_source` is `"url"`. In that case it is still passed to the HTTP client for socket binding. Set it to `""` if interface binding is not needed.

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
- `check_response(response)` — receives the raw HTTP response body string, returns `true` if the update was successful.
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

The `master` branch (v1.x) offers:
- **C++23** with modern standard library features
- **Significantly better performance** through a rewritten scheduler and optimized HTTP layer
- **New driver architecture** with `HttpClient` abstraction, multi-step workflow support, and glaze-based config parsing
- **New features** not available in this legacy branch

Switch to `master` if your system can meet the build requirements (GCC 14+, CMake 3.28+).

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
