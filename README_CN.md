# yaddnsc — Yet Another Dynamic DNS Client

[![许可证：MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![CI](https://github.com/CyberKoo/yaddnsc/actions/workflows/ci.yml/badge.svg)](https://github.com/CyberKoo/yaddnsc)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![codecov](https://codecov.io/github/CyberKoo/yaddnsc/graph/badge.svg?token=OA6OJQ3MN6)](https://codecov.io/github/CyberKoo/yaddnsc)
![Linux](https://img.shields.io/badge/Linux-glibc%20%7C%20musl-FCC624?logo=linux&logoColor=black)
![macOS](https://img.shields.io/badge/macOS-arm64-000000?logo=apple)

> **发布状态：** 当前版本为 `v1.0.0-alpha.2`（预发布）。`master` 分支对应已发布版本，
> `dev` 分支汇集面向下一版本的变更，`v0.x` 分支继续支持旧工具链。驱动插件的 ABI
> 在不同构建之间可能发生变化，升级宿主程序后须重新编译外部构建的驱动。

yaddnsc 是一个动态 DNS 客户端。它按配置周期性地获取每条记录的 IP 地址——来源可以
是本地网络接口、HTTP(S) 端点或组播 DNS——并与 DNS 中当前发布的地址进行比对；两者
不一致时，通过对应服务商的驱动提交更新。全部域名由一份 JSON 配置文件管理，每条记录
可以独立设置记录类型、地址来源与更新间隔。

## 目录

- [功能特性](#功能特性)
- [安装](#安装)
- [快速开始](#快速开始)
- [命令行用法](#命令行用法)
- [配置文件](#配置文件)
- [IP 地址来源](#ip-地址来源)
- [DNS 解析器](#dns-解析器)
- [TLS 与 CA 证书](#tls-与-ca-证书)
- [以服务方式运行](#以服务方式运行)
- [故障排查](#故障排查)
- [其他文档](#其他文档)
- [许可证](#许可证)

## 功能特性

- 通过一份 JSON 配置管理多个域名与多条记录。
- 每条记录可独立设置记录类型（A/AAAA）、地址来源与更新间隔，并可配置周期性的强制
  更新。
- 地址来源包括本地网络接口、HTTP(S) 端点与 mDNS。
- 随附十二个以可加载模块形式提供的服务商驱动；亦可基于随安装的 SDK 开发第三方
  驱动。
- DNS 解析支持 UDP/TCP、DNS-over-HTTPS 与 DNS-over-TLS，并提供并发、回退、随机
  三种查询策略。
- 更新循环启动前对配置与驱动参数进行校验。
- 收到 SIGINT/SIGTERM 后执行有序退出，并取消进行中的网络请求。
- 支持 Linux（glibc 与 musl）及 macOS（arm64）。

各驱动的参数与凭据要求见 [DRIVERS_CN.md](DRIVERS_CN.md)（[English](DRIVERS.md)）。

## 安装

目前不提供预编译软件包。可选方式为从源码构建、生成 Debian 软件包或构建容器镜像。

### 从源码构建

构建要求：

- CMake 3.28 或更高版本
- OpenSSL 3.0 或更高版本
- 支持 C++23 的编译器：GCC 14+、Clang 19+ 或 Apple Clang 15+
- libxml2（可选；缺失时将不构建 `namecheap` 与 `route53` 驱动）

其余依赖由构建系统自动获取。

Debian/Ubuntu：

```bash
sudo apt install build-essential cmake pkg-config libssl-dev
```

macOS：

```bash
brew install cmake pkg-config openssl@3
```

配置、构建并安装：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
sudo cmake --install build
```

未指定 `CMAKE_BUILD_TYPE` 时默认采用 Debug 构建，生产环境应显式指定 `Release`。
安装布局为：可执行文件位于前缀的 `bin` 目录，驱动模块位于前缀的库目录（默认
安装时为 `<prefix>/lib/yaddnsc/drivers`），示例配置位于
`<sysconfdir>/yaddnsc/config.json`。可通过 `-DCMAKE_INSTALL_PREFIX=...` 更改
安装前缀。

### Debian 软件包

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DYADDNSC_ENABLE_DEB=ON
cmake --build build --parallel
cpack --config build/CPackConfig.cmake -G DEB
```

也可以使用 `docker/build-deb.sh` 在 Ubuntu 容器内完成构建（默认 24.04），产物输出
至 `deb-out/`；可用选项请参阅脚本头部注释。

### 容器镜像

仓库根目录的 Dockerfile 用于构建基于 Alpine 的运行时镜像：

```bash
docker build -t yaddnsc .
docker run -d --name yaddnsc \
  -v /etc/yaddnsc/config.json:/etc/yaddnsc/config.json:ro \
  yaddnsc
```

镜像的入口为 `yaddnsc` 可执行文件，默认命令为 `run -c /etc/yaddnsc/config.json`。

## 快速开始

创建 `config.json`，校验通过后启动客户端：

```bash
yaddnsc config test
yaddnsc run
```

以下为使用 Cloudflare 驱动的最简配置：

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

除 Route 53 外，所有随附驱动仅更新已存在的记录。请先在服务商处创建记录，再将其
分配的标识（如 `zone_id`、`record_id`）填入 `driver_param`。各驱动的参数说明见
[DRIVERS_CN.md](DRIVERS_CN.md)。

## 命令行用法

```bash
# 启动更新循环（默认读取 ./config.json）
yaddnsc run

# 指定配置文件并开启调试日志
yaddnsc run -c /etc/yaddnsc/config.json -d

# 校验配置；-q 抑制成功提示
yaddnsc config test -q

# 查看配置、驱动、网络接口与解析器设置
yaddnsc config show
yaddnsc driver list
yaddnsc driver info <name>
yaddnsc interface list
yaddnsc interface ip <name>
yaddnsc dns resolver

# 执行一次性的 DNS 查询
yaddnsc dns resolve <hostname> --type A

# 输出构建信息或版本号
yaddnsc info
yaddnsc --version
```

`-c/--config` 选项定义在具体命令上（`run`、`config test`、`config show`、
`driver list`、`driver info`、`dns resolve`、`dns resolver`），因此须置于命令名
之后。`interface` 子命令与 `info` 不读取配置文件。

命令执行失败时以非零状态退出。`dns resolve` 例外：查询失败仅在输出中报告，退出
状态仍为 0；仅用法错误返回非零状态。

安装软件包或执行 `cmake --install` 时会一并安装 bash、zsh 与 fish 的补全文件。
若补全未生效，请重新启动 shell 会话。

## 配置文件

未使用 `-c` 指定时，yaddnsc 读取当前目录下的 `./config.json`：

```bash
yaddnsc run -c /etc/yaddnsc/config.json
yaddnsc config test -c /etc/yaddnsc/config.json -q
```

由于 `-c` 属于叶子命令，`yaddnsc config -c ... test` 不是合法用法；`config` 仅为
命令分组。

### 配置结构

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

### 字段说明

| 对象 | 字段 | 说明 |
|---|---|---|
| `driver` | `driver_dir` | 驱动模块的搜索目录；省略时使用安装时确定的驱动目录。 |
| `driver` | `auto_discover` | 加载 `driver_dir` 中的全部模块；启用后 `load` 列表被忽略。 |
| `driver` | `load` | 显式指定待加载的模块列表，例如 `["cloudflare.so"]`；模块加载失败属于致命错误。 |
| `resolver` | `use_custom_server` | 使用下方配置的服务器替代内置默认服务器，详见 [DNS 解析器](#dns-解析器)。 |
| `resolver` | `servers` | DNS 服务器列表；优先于旧版 `address`/`port` 字段。 |
| `resolver` | `strategy` | `concurrent`（默认）、`fallback` 或 `shuffle`。 |
| `domains[]` | `name` | 受管理的域名，例如 `example.com`。 |
| `domains[]` | `update_interval` | 更新间隔，单位秒；默认下限为 60，可在构建时调整。 |
| `domains[]` | `force_update` | 强制更新间隔，单位秒；到达该间隔时不与 DNS 比对即推送更新。`0`（默认）表示关闭；非零时不得小于 `update_interval`。 |
| `domains[]` | `driver` | 处理该域名的驱动模块名称。 |
| `domains[]` | `subdomains` | 该域名下受管理的记录，至少一条。 |
| `subdomains[]` | `name` | 记录标签；`@` 表示域名顶点（apex）记录。 |
| `subdomains[]` | `type` | `a` 或 `aaaa`；省略时按 `a` 处理。`txt` 可被配置接受，但没有任何随附驱动支持更新 TXT 记录。 |
| `subdomains[]` | `ip_source` | `interface`、`http` 或 `mdns`，详见 [IP 地址来源](#ip-地址来源)。 |
| `subdomains[]` | `ip_source_param` | `http` 来源使用 URL，`mdns` 来源使用主机名；`interface` 来源不使用该字段。 |
| `subdomains[]` | `interface` | 网络接口名称；`interface` 来源必填，其余网络来源可选。 |
| `subdomains[]` | `update_interval` | 单条记录的更新间隔，单位秒；`0`（默认）表示继承域名级间隔。 |
| `subdomains[]` | `allow_ula` | 允许接口来源采用 IPv6 唯一本地地址（fc00::/7），默认 `false`。 |
| `subdomains[]` | `allow_local_link` | 允许接口来源采用 IPv6 链路本地地址（fe80::/10），默认 `false`。 |
| `subdomains[]` | `driver_param` | 驱动专有参数，仅在子域名层级有效，详见 [DRIVERS_CN.md](DRIVERS_CN.md)。 |

### 凭据保护

`driver_param` 通常包含 API 令牌或密钥。请勿将配置文件纳入版本控制，应限制其访问
权限，并且仅为每个令牌授予完成更新所需的最小权限：

```bash
chmod 600 /etc/yaddnsc/config.json
```

作为防止意外泄露的措施，`config show` 会将 `driver_param` 中键名包含 `token`、
`password`、`secret` 或 `key`（不区分大小写）的值显示为 `"***"`；磁盘上的配置文件
仍保存真实值。

## IP 地址来源

### `interface`

读取 `interface` 字段指定的本地网络接口的地址，该字段对此来源为必填。A 记录取
IPv4 地址，AAAA 记录取 IPv6 地址。除非设置 `allow_ula` 或 `allow_local_link`，
IPv6 唯一本地地址与链路本地地址不参与选取。

### `http`

向 `ip_source_param` 指定的 URL 发起请求，并将响应正文按纯文本 IP 地址解析。建议
使用 HTTPS 端点。可选的 `interface` 字段将出站请求绑定到指定接口，实际效果受系统
路由与权限约束。

### `mdns`

通过组播 DNS 解析 `ip_source_param` 指定的主机名，该名称须以 `.local` 结尾；仅
支持 `a` 与 `aaaa` 记录。此来源面向局域网设备，依赖组播流量，因此在容器、VPN
隧道及云网络中通常不可用。可选的 `interface` 字段指定查询所用接口，IPv6 场景下
可能需要显式指定。

## DNS 解析器

未配置自定义服务器时，解析器使用单个内置服务器，默认为 `1.1.1.1:53`，可在构建时
通过 `-DYADDNSC_DEFAULT_DNS_SERVER=...` 与 `-DYADDNSC_DEFAULT_DNS_PORT=...` 修改。

将 `use_custom_server` 设为 `true` 即以配置的服务器列表取代内置服务器。此时须通过
`servers` 数组或旧版 `address`/`port` 字段提供至少一个服务器；自定义服务器为空
属于非法配置，`run` 与 `config test` 均会校验失败。`servers` 存在时优先于旧版
字段；`use_custom_server` 为 `false` 时，全部自定义字段均被忽略。

服务器条目的 `address` 形式决定所用协议：

| 形式 | 协议 | 端口 |
|---|---|---|
| 纯 IP 或主机名，配合 `port` | UDP 上的 DNS，必要时回退 TCP | `port`（默认 53） |
| `https://host/path` | DNS-over-HTTPS | 取自 URI，默认 443 |
| `tls://host[:port]` | DNS-over-TLS | 取自 URI，默认 853 |

采用 URI 形式时，条目中的 `port` 字段被忽略。

配置多个服务器时，`strategy` 决定查询顺序：

| 策略 | 行为 |
|---|---|
| `concurrent` | 并行查询各解析器，采用最先返回的成功结果。 |
| `fallback` | 按配置顺序依次查询。 |
| `shuffle` | 每次查询随机排列顺序后依次查询。 |

## TLS 与 CA 证书

服务器证书校验始终启用。CA 证书包按以下顺序确定：`SSL_CERT_FILE` 环境变量、
OpenSSL 默认位置、若干平台相关的系统路径。若未找到任何证书包，TLS 连接将失败，
而不会退化为不校验的连接。不支持 `SSL_CERT_DIR`；如需附加证书，请将其合并为单一
PEM 证书包，并通过 `SSL_CERT_FILE` 指定：

```bash
export SSL_CERT_FILE=/etc/ssl/private/company-ca-bundle.pem
yaddnsc run
```

## 以服务方式运行

安装时若系统中存在 systemd 开发文件，则会安装相应的 systemd 单元；Debian 软件包
始终包含该单元。单元在启动前校验配置，失败时自动重启，日志输出至 journal，并以
动态分配的用户在受限权限下运行。

```bash
yaddnsc config test -c /etc/yaddnsc/config.json
sudo systemctl daemon-reload
sudo systemctl enable --now yaddnsc
sudo systemctl status yaddnsc
journalctl -u yaddnsc
```

单元默认读取 `/etc/yaddnsc/config.json`。如需使用其他路径，可在可选环境文件
`/etc/yaddnsc/default/yaddnsc` 中设置 `YADDNSC_CONFIG`。由于服务以动态用户运行，
配置文件须对服务可读，同时仍应拒绝无关账户访问。

## 故障排查

| 现象 | 建议检查 |
|---|---|
| 找不到驱动 | 执行 `yaddnsc driver list`，确认 `driver_dir` 设置与模块安装情况。 |
| 配置被拒绝 | 执行 `yaddnsc config test`，查看具体校验错误。 |
| DNS 查询失败 | 执行 `yaddnsc dns resolver`，检查服务器地址、端口与防火墙规则。 |
| HTTP 来源失败 | 确认端点返回纯文本 IP 地址，且可通过 HTTPS 正常访问。 |
| TLS 校验失败 | 检查系统时间、CA 证书包与 `SSL_CERT_FILE`。 |
| mDNS 无应答 | 确认 `.local` 名称、组播可用性与所选接口。 |
| 服务启动失败 | 查看 `systemctl status yaddnsc` 与 `journalctl -u yaddnsc`。 |
| 升级后驱动被拒绝 | 使用当前 SDK 重新编译驱动；宿主要求 ABI 修订号完全一致。 |

## 其他文档

- [DNS 服务商驱动](DRIVERS_CN.md)
- [自定义驱动与 ABI 兼容性](docs/custom-drivers.md)
- [开发、测试与覆盖率](docs/development.md)
- [架构说明](docs/architecture.md)
- [文档索引](docs/README.md)

## 许可证

本项目依据 MIT 许可证发布，详见 [LICENSE](LICENSE)。
