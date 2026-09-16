# yaddnsc — Yet Another Dynamic DNS Client

[![许可证：MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![CI](https://github.com/CyberKoo/yaddnsc/actions/workflows/ci.yml/badge.svg)](https://github.com/CyberKoo/yaddnsc)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![codecov](https://codecov.io/github/CyberKoo/yaddnsc/graph/badge.svg?token=OA6OJQ3MN6)](https://codecov.io/github/CyberKoo/yaddnsc)
![Linux](https://img.shields.io/badge/Linux-glibc%20%7C%20musl-FCC624?logo=linux&logoColor=black)
![macOS](https://img.shields.io/badge/macOS-arm64-000000?logo=apple)
![FreeBSD](https://img.shields.io/badge/FreeBSD-supported-AB2B28?logo=freebsd)

> **状态：** `master` 是发布分支（当前为 `v1.0.0-alpha.2`）。`dev` 包含此发布
> 之后的变更，`v0.x` 用于旧工具链维护。v1 仍是预发布软件；升级 build 或更换工具链
> 后必须重新编译驱动插件。

**yaddnsc** 监控本地或外部获取的 IP 地址，并在地址变化时更新 DNS 记录。它支持
多域名、IPv4/IPv6 记录、多种 DNS 解析协议以及 12 个内置服务商驱动。

## 目录

- [功能特性](#功能特性)
- [安装](#安装)
- [快速开始](#快速开始)
- [使用方法](#使用方法)
- [配置文件](#配置文件)
- [IP 来源](#ip-来源)
- [DNS 解析器](#dns-解析器)
- [TLS 和 CA 证书](#tls-和-ca-证书)
- [生产部署](#生产部署)
- [故障排查](#故障排查)
- [开发者文档](#开发者文档)
- [许可证](#许可证)

## 功能特性

- 使用一个 JSON 配置管理多个域名和子域名。
- 独立配置 A、AAAA 记录和更新间隔。
- 从本地网卡、HTTP(S) 端点或 mDNS 获取地址。
- 内置 12 个 DNS 服务商驱动。
- 支持传统 DNS、DNS-over-HTTPS 和 DNS-over-TLS。
- 支持并发、回退和随机顺序 DNS 查询策略。
- 收到 SIGINT/SIGTERM 后优雅退出，并取消正在进行的网络请求。
- 支持 Linux（glibc/musl）、macOS 和 FreeBSD。

各服务商的具体参数和凭据要求见 [DRIVERS_CN.md](DRIVERS_CN.md)。

## 安装

目前尚未发布预编译包。可以从源码构建、生成 Debian 软件包，或使用项目提供的
Dockerfile。

### 从源码构建

需要 CMake 3.28+、OpenSSL 3.0+，以及支持 C++23 的编译器：GCC 14+、Clang 19+
或 Apple Clang 15+。

Debian/Ubuntu：

```bash
sudo apt install build-essential cmake pkg-config libssl-dev
```

macOS：

```bash
brew install cmake pkg-config openssl@3
```

构建并安装：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo cmake --install build
```

默认情况下，二进制文件安装到所选前缀，驱动安装到
`${libdir}/yaddnsc/drivers`，系统配置文件安装到
`${sysconfdir}/yaddnsc/config.json`（通常是 `/etc/yaddnsc/config.json`）。如需更换
前缀，请在配置阶段使用 `-DCMAKE_INSTALL_PREFIX=...`。

### Debian 软件包

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DYADDNSC_ENABLE_DEB=ON
cmake --build build --parallel
cpack --config build/CPackConfig.cmake -G DEB
```

也可以使用 Docker 构建：

```bash
./docker/build-deb.sh
```

### Docker

```bash
docker build -t yaddnsc .
docker run --rm yaddnsc --help
```

## 快速开始

创建 `config.json`，验证配置，然后运行客户端：

```bash
yaddnsc config test
yaddnsc run
```

下面是**配置结构示例**。其中 `simple` URL 是占位地址，必须替换为真正执行 DNS
更新的 API 端点；按原样使用不会更新真实记录。

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

使用真实服务商时，请从 [DRIVERS_CN.md](DRIVERS_CN.md) 选择驱动并复制对应的
`driver_param` 示例。启动更新循环前请先执行 `config test`。

## 使用方法

```bash
# 使用 ./config.json 运行
yaddnsc run

# 指定配置文件并启用调试日志
yaddnsc run -c /etc/yaddnsc/config.json -d

# 验证配置；-q/--quiet 不显示成功提示
yaddnsc config test -q

# 查看配置、驱动、网卡或解析器信息
yaddnsc config show
yaddnsc driver list
yaddnsc driver info <name>
yaddnsc interface list
yaddnsc interface ip <name>
yaddnsc dns resolver

# 解析主机名
yaddnsc dns resolve <hostname> --type A

yaddnsc info
yaddnsc --version
yaddnsc --help
```

命令失败时返回非零退出状态。诊断命令报告 DNS 查询失败，但不会启动更新循环。

### Shell 自动补全

安装软件包或执行 `cmake --install` 时会安装 bash、zsh 和 fish 补全文件。如果补全
未立即生效，请重启 shell。

## 配置文件

yaddnsc 默认读取 `./config.json`。运行命令或诊断子命令可以使用 `-c` 指定其他文件。

### 基本结构

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

### 通用字段

| 对象 | 字段 | 说明 |
|---|---|---|
| `driver` | `driver_dir` | 驱动库目录；省略时使用安装目录。 |
| `driver` | `auto_discover` | 加载目录中的所有 `.so`；启用时忽略 `load`。 |
| `driver` | `load` | 手动加载的驱动库名称。 |
| `resolver` | `use_custom_server` | 使用配置的服务器，而不是构建时默认值。 |
| `resolver` | `servers` | DNS 服务器列表，见 [DNS 解析器](#dns-解析器)。 |
| `resolver` | `strategy` | `concurrent`、`fallback` 或 `shuffle`。 |
| `domains[]` | `name` | 要管理的域名，例如 `example.com`。 |
| `domains[]` | `update_interval` | 默认更新间隔（秒），必须满足构建时的最小值。 |
| `domains[]` | `force_update` | 周期性强制更新间隔；`0` 表示关闭。 |
| `domains[]` | `driver` | 已加载的驱动名称。 |
| `domains[]` | `subdomains` | 此域名下需要管理的记录。 |
| `subdomains[]` | `name` | 子域名标签；使用 `@` 表示根域名记录。 |
| `subdomains[]` | `type` | 记录类型。DDNS 更新请使用 `a` 或 `aaaa`。`txt` 可用于 `dns resolve`，更新能力因驱动而异，目前仅 Cloudflare 驱动有明确文档说明。 |
| `subdomains[]` | `ip_source` | `interface`、`http` 或 `mdns`。 |
| `subdomains[]` | `ip_source_param` | `http` 使用 URL，`mdns` 使用主机名；`interface` 不使用。 |
| `subdomains[]` | `interface` | 网卡名称；interface 来源必填，其他网络来源可选。 |
| `subdomains[]` | `update_interval` | 单条记录的间隔；省略或为 `0` 时继承域名设置。 |
| `subdomains[]` | `driver_param` | 服务商专用设置，见 [DRIVERS_CN.md](DRIVERS_CN.md)。 |

`allow_ula` 和 `allow_local_link` 控制 IPv6 interface 来源是否接受对应地址范围。
新配置请使用上述字段；旧字段仅用于兼容。

不要把凭据提交到源码仓库，并限制配置文件权限：

```bash
chmod 600 /etc/yaddnsc/config.json
```

使用服务商要求的最小权限。`config show` 默认对敏感 `driver_param` 字段脱敏：键名
（转小写后）包含 `token`、`password`、`secret` 或 `key` 的成员一律显示为 `"***"`。
配置文件本身仍保存真实值——请按上文继续限制文件权限，分享日志前也请先检查。

## IP 来源

### `interface`

从本地网卡读取地址。A 记录使用 IPv4，AAAA 记录使用 IPv6。网卡必须存在，并且服务
进程有权限使用它。

### `http`

从响应正文为纯 IP 地址的 HTTP(S) 端点获取地址。推荐使用 HTTPS。可选的 `interface`
控制出站网卡，但实际行为还受系统路由和权限影响。

### `mdns`

使用 multicast DNS 查询局域网中的 `.local` 主机名，例如 `printer.local`。它用于
局域网设备，不是公网 DNS。容器、VPN、云主机或禁止 multicast 的网络可能无法使用；
IPv6 mDNS 可能需要指定网卡。

## DNS 解析器

启用 `use_custom_server` 时使用配置的服务器，否则使用构建时默认值，通常为
`1.1.1.1:53`。维护者可以在配置构建时使用
`-DYADDNSC_DEFAULT_DNS_SERVER=...` 和 `-DYADDNSC_DEFAULT_DNS_PORT=...` 修改默认值。

- **传统 DNS：** 使用 IP 地址和 `port`；UDP 失败或响应过大时使用 TCP。
- **DoH：** 使用完整的 `https://host/path` 地址，例如
  `https://1.1.1.1/dns-query`；端口从 URI 读取，默认 `443`。
- **DoT：** 使用 `tls://host[:port]` 地址；端口从 URI 读取，默认 `853`。

DoH 和 DoT 会忽略 server 对象中的 `port` 字段。

| 策略 | 行为 |
|---|---|
| `concurrent` | 并行查询解析器，并返回第一个成功结果。 |
| `fallback` | 按配置顺序依次尝试。 |
| `shuffle` | 每次随机化顺序后依次尝试。 |

## TLS 和 CA 证书

TLS 证书校验默认开启。使用私有 CA bundle 时，在启动前设置 `SSL_CERT_FILE`：

```bash
export SSL_CERT_FILE=/etc/ssl/private/company-ca-bundle.pem
yaddnsc config test
yaddnsc run
```

证书包依次从 `SSL_CERT_FILE`、OpenSSL 默认路径和平台系统路径中选择，并在进程生命
周期内缓存。如果没有信任库，证书校验仍保持开启，TLS 连接会安全失败。不支持
`SSL_CERT_DIR`；请改用合并后的 PEM bundle。

## 生产部署

安装后，先验证配置，再启用服务。源码安装仅在配置阶段能找到 systemd 开发元数据时
才会安装 systemd unit；Debian 软件包始终包含该 unit。

```bash
yaddnsc config test
sudo systemctl daemon-reload
sudo systemctl enable --now yaddnsc
sudo systemctl status yaddnsc
journalctl -u yaddnsc
```

服务启动前会验证配置。安装后的系统配置通常位于 `/etc/yaddnsc/config.json`。如果软件
包提供配置路径环境覆盖，请参考已安装的服务说明，并确保配置文件仅服务账号可读。

## 故障排查

| 现象 | 首先检查 |
|---|---|
| 找不到驱动 | 执行 `yaddnsc driver list`，检查 `driver_dir` 和安装结果。 |
| 配置被拒绝 | 执行 `yaddnsc config test`，检查驱动名、记录类型和更新间隔。 |
| DNS 查询失败 | 执行 `yaddnsc dns resolver`，检查服务器地址、端口和防火墙。 |
| HTTP 来源失败 | 确认端点只返回 IP 地址，并检查 HTTPS 和路由。 |
| TLS 校验失败 | 检查系统时间、CA bundle 和 `SSL_CERT_FILE`。 |
| mDNS 无响应 | 检查 `.local` 名称、multicast、网卡、容器网络和防火墙。 |
| systemd 启动失败 | 执行 `systemctl status yaddnsc` 和 `journalctl -u yaddnsc`。 |
| ABI 不兼容 | 使用相同 yaddnsc/工具链重新编译或安装驱动。 |

## 开发者文档

- [DNS 服务商驱动](DRIVERS_CN.md)
- [自定义驱动和 ABI 兼容性](docs/custom-drivers.md)
- [开发、测试和覆盖率](docs/development.md)
- [架构说明](docs/architecture.md)
- [文档地图](docs/README.md)

## 许可证

本项目遵循 [LICENSE](LICENSE) 文件中规定的 MIT 许可证。
