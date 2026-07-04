# yaddnsc — Yet Another Dynamic DNS Client（legacy 分支）

**yaddnsc** 是一款动态 DNS（DDNS）客户端，可以监控本机 IP 地址的变化，并在检测到变动时通过插件式驱动自动更新所支持的 DNS 服务商上的域名解析记录。它设计轻量、模块化，并可通过驱动插件系统进行扩展。

> **这是 `legacy` 分支**，专门针对老旧系统进行移植。
> - C++ 标准降至 **C++17**，可在 **Ubuntu 20.04** 上直接编译（GCC 9 + OpenSSL 1.1.x，系统自带源，无需 PPA）。
> - **ABI 兼容 v0.x 驱动插件**，现有的 `.so` 驱动无需重新编译即可使用。
> - **仅维护** — 此分支不再添加新功能，仅修复重大 Bug。**新功能和更好的性能请见 `master` 分支（v1.x）。**
> - DoH（DNS-over-HTTPS）和 DoT（DNS-over-TLS）解析器支持也已从 master 分支移植至此。

## 功能特性

- **多域名与多子域名管理** — 单个配置文件即可管理多个域名及其子域名。
- **插件化驱动架构** — 驱动以共享库（`.so`）形式提供，在运行时通过 `dlopen` 动态加载。内置驱动包括：
  - [Cloudflare](https://www.cloudflare.com/) — 通过 Cloudflare API v4 更新 DNS 记录
  - [DigitalOcean](https://www.digitalocean.com/) — 通过 DigitalOcean API v2 更新 DNS 记录
  - [DNSPod](https://www.dnspod.com/) — 通过 DNSPod API 更新 DNS 记录（同时支持国内和国际端点）
  - [Simple](https://github.com/Kotarou/yaddnsc) — 通用 HTTP 驱动，支持 URL 模板替换，适用于自定义 API 端点
- **灵活的 IP 获取方式** — 每个子域名可独立选择：
  - `interface` — 从本地网络接口获取 IP 地址
  - `url` — 从外部 HTTP 服务获取 IP 地址（如 `https://ifconfig.me`）
- **IPv4 和 IPv6 支持** — 可独立配置 A 和 AAAA 记录。
- **自定义 DNS 解析器** — 可选择使用特定的 DNS 服务器，而非系统默认解析器。支持 **传统 DNS**（纯 IP + 端口）、**DNS-over-HTTPS (DoH)**（完整的 HTTPS URL，如 `https://1.1.1.1/dns-query`）和 **DNS-over-TLS (DoT)**（`tls://` 协议 URI，如 `tls://1.1.1.1`）。协议会根据地址前缀自动识别。
- **强制更新调度** — 即使 IP 未发生变化，也可按设定周期强制更新 DNS 记录。
- **优雅退出** — 通过专用信号处理线程处理 SIGINT/SIGTERM 信号。
- **基于线程池的并发处理** — 子域名更新任务通过线程池并行执行。

## 构建要求

### 前置依赖

| 工具/库      | 最低版本    |
|------------|-----------|
| CMake      | 3.14      |
| C++ 编译器   | 支持 C++17 |
| OpenSSL    | 1.1.x     |
| Zlib       | 任意较新版本  |

yaddnsc 仅支持 POSIX 系统。支持的编译器：GCC 9+、Clang 10+

### 编译方法

```bash
# 安装系统依赖（Ubuntu 20.04）
sudo apt install libssl-dev zlib1g-dev build-essential cmake

# 克隆仓库（包含子模块）
git clone --recursive -b v0.x https://github.com/Kotarou/yaddnsc.git
cd yaddnsc

# 编译
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 主程序位于 build/objs/yaddnsc
# 驱动模块位于 build/objs/driver/*.so
```

### CMake 选项

| 选项                  | 默认值    | 说明                              |
|---------------------|--------|---------------------------------|
| `CMAKE_BUILD_TYPE` | Release | 设为 `Debug` 生成调试版本               |
| `LIBC_MUSL`        | OFF    | 启用 musl 特定的兼容处理                |
| `NO_RTTI`          | OFF    | 禁用 RTTI 以减小二进制体积（`-fno-rtti`） |

第三方依赖（spdlog、cpp-httplib v0.14.3、cxxopts、BS::thread_pool、fmt、nlohmann_json）以 **git 子模块** 方式管理（而非通过 CPM 获取）。

### 驱动插件 ABI 兼容性

为 **yaddnsc v0.x** 编译的驱动与此 legacy 分支二进制兼容。驱动 ABI 版本号（`DRV_VERSION`）保持不变，仍为 `"1000000"`。将现有的 `.so` 文件放入驱动目录即可直接使用，无需重新编译。

## 配置文件说明

yaddnsc 使用 JSON 格式的配置文件。默认查找当前目录下的 `./config.json`，也可通过 `-c` 参数指定其他路径。

### 配置示例

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

> **DoH 示例：** 要使用 DNS-over-HTTPS，将 `ipaddress` 设置为完整的 HTTPS URL：
> ```json
> { "ipaddress": "https://1.1.1.1/dns-query", "port": 443 }
> ```
> 地址必须以 `https://` 开头并包含完整路径（通常为 `/dns-query`）。协议会根据地址前缀自动识别，因此无需显式设置 `"protocol": "doh"`。

> **DoT 示例：** 要使用 DNS-over-TLS，使用 `tls://` 前缀：
> ```json
> { "ipaddress": "tls://1.1.1.1" }
> ```

### 配置字段参考

#### 顶层字段

| 字段        | 类型     | 说明                 |
|-----------|--------|--------------------|
| `driver`  | object | 驱动加载配置             |
| `resolver`| object | 自定义 DNS 解析器设置（可选）  |
| `domains` | array  | 域名配置列表             |

#### `driver` 对象

| 字段           | 类型       | 说明                               |
|--------------|----------|----------------------------------|
| `driver_dir` | string   | 驱动 `.so` 文件所在目录（可选）              |
| `load`       | string[] | 需要加载的驱动共享库文件名列表                 |

#### `resolver` 对象

| 字段                 | 类型      | 说明                                                      |
|--------------------|---------|---------------------------------------------------------|
| `use_custom_server` | boolean | 为 true 时使用指定的 DNS 服务器，否则使用系统默认                    |
| `ipaddress`        | string  | DNS 服务器地址：纯 IP（如 `1.1.1.1`）、DoH 的 HTTPS URL 或 `tls://` URI |
| `port`             | integer | DNS 端口，默认 53（可选）                                      |
| `protocol`         | string  | DNS 协议：`"system"`（默认）、`"doh"` 或 `"dot"`（可选；未设置时根据地址自动识别） |

#### `domains[]` 对象

| 字段               | 类型     | 说明                          |
|------------------|--------|-----------------------------|
| `name`           | string | 域名（如 `example.com`）         |
| `update_interval` | int    | 更新间隔（秒），最小 60              |
| `force_update`   | int    | 强制更新间隔（秒），0 表示禁用           |
| `driver`         | string | 使用的驱动名称（必须与已加载的驱动匹配）       |
| `subdomains`     | array  | 子域名记录列表                     |

#### `subdomains[]` 对象

| 字段                | 类型      | 说明                                                |
|-------------------|---------|---------------------------------------------------|
| `name`            | string  | 子域名名称（如 `home` 对应 `home.example.com`）               |
| `type`            | string  | DNS 记录类型：`"a"`、`"aaaa"`、`"txt"` 或 `"soa"`          |
| `interface`       | string  | 网络接口名称（如 `eth0`）。**始终必填**，即使 `ip_source` 为 `"url"` 也需设置 |
| `ip_type`         | string  | IP 版本：`"ipv4"`、`"ipv6"` 或 `"unspecified"`（可选，默认 `"unspecified"`） |
| `ip_source`       | string  | IP 来源：`"interface"`（从本地网络接口获取）或 `"url"`（通过 HTTP 获取）   |
| `ip_source_param` | string  | `ip_source` 为 `"url"` 时的 HTTP(S) URL                 |
| `allow_ula`       | boolean | 是否允许 ULA 地址（仅 IPv6），默认 `false`（可选）                 |
| `allow_local_link`| boolean | 是否允许链路本地地址（仅 IPv6），默认 `false`（可选）                |
| `driver_param`    | object  | 驱动特定参数（键值对）                                         |

> **注意：** `interface` 始终为必填字段。当 `ip_source` 为 `"url"` 时，该字段仍会传递给 HTTP 客户端用于 socket 绑定。若不需要接口绑定，请将其设为 `""`。

## 编写自定义驱动

### 驱动 API

驱动需实现 `IDriver` 接口（定义在 `include/IDriver.h`）：

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

- `generate_request(config)` — 接收子域名配置中的 `driver_param` 映射，返回 `driver_request` 结构体（包含 URL、请求体、Content-Type、HTTP 方法、请求头）。
- `check_response(response)` — 接收 HTTP 响应体字符串，返回 `true` 表示更新成功。
- `get_detail()` — 返回驱动元信息（名称、描述、作者、版本）。
- `get_driver_version()` — 返回 ABI 版本常量。`BaseDriver` 已提供 `final` 实现，请勿重写。

`BaseDriver` 类（位于 `driver/base_driver.h`）提供了以下辅助方法：

| 方法                       | 说明                                |
|--------------------------|-----------------------------------|
| `check_required_params()` | 验证 `driver_param` 中是否存在必需的键        |
| `get_optional()`         | 安全地从 `driver_param` 获取可选参数        |
| `vformat(format, args)`  | 支持命名参数（`std::map`）或位置参数（`std::vector`）的字符串格式化 |

### 驱动工厂函数

每个驱动必须以 C 链接导出 `create()` 工厂函数：

```cpp
// cloudflare.h
extern "C" inline IDriver *create() {
    return new CloudflareDriver;
}
```

将以上代码放在驱动的头文件中。核心程序通过 `dlopen` 加载驱动并查找 `create()` 符号。

### 编译为共享库

驱动需编译为 `MODULE` 类型的共享库（生成位置无关代码，不加 `lib` 前缀）：

```cmake
add_library(cloudflare MODULE cloudflare.cpp cloudflare.h)
target_link_libraries(cloudflare PRIVATE yaddnsc_lib)
```

## 升级到 v1.x

`master` 分支（v1.x）提供：
- **C++23** 标准及其现代标准库特性
- **显著更佳的性能** — 重写的调度器和优化的 HTTP 层
- **全新的驱动架构** — 支持 `HttpClient` 抽象、多步骤工作流和 glaze 配置解析
- **新功能** — legacy 分支不提供的新特性

如果你的系统满足构建要求（GCC 14+、CMake 3.28+），建议切换至 `master` 分支。

## 依赖项

此分支使用 **git 子模块**管理依赖（而非 CPM）。

| 库                                                              | 用途            | 管理方式      |
|---------------------------------------------------------------|---------------|-----------|
| [spdlog](https://github.com/gabime/spdlog)                    | 日志记录          | submodule |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) v0.14.3 | HTTP 客户端（兼容 OpenSSL 1.1.x） | submodule |
| [cxxopts](https://github.com/jarro2783/cxxopts)               | 命令行参数解析       | submodule |
| [BS::thread_pool](https://github.com/bshoshany/thread-pool)   | 线程池           | submodule |
| [fmt](https://github.com/fmtlib/fmt)                          | 字符串格式化        | submodule |
| [nlohmann_json](https://github.com/nlohmann/json)             | JSON 解析       | submodule |
| OpenSSL (1.1.x)                                                | TLS 支持        | 系统库      |
| Zlib                                                           | 压缩            | 系统库      |

## 许可证

本项目遵循 [LICENSE](LICENSE) 文件中的许可条款。
