# yaddnsc — Yet Another Dynamic DNS Client（v0.x，旧版维护分支）

> **分支状态。** 本分支为旧版维护分支，面向工具链无法满足 v1.x 构建要求的系统：语言
> 标准降至 C++17，并支持 OpenSSL 1.1.x。本分支仅接受缺陷修复，不再引入新功能。
> 为 v0.x 构建的驱动插件与本分支保持二进制兼容；为 v1.x 构建的插件会被拒绝加载。
> 新功能的开发在 `master` 分支（v1.x）进行。

yaddnsc 是一个动态 DNS 客户端。它按配置周期性地获取每条记录的 IP 地址——来源可以
是本地网络接口或 HTTP(S) 端点——并与 DNS 中当前发布的地址进行比对；两者不一致时，
通过以共享库形式加载的服务商驱动提交更新。全部域名由一份 JSON 配置文件管理。

## 目录

- [功能特性](#功能特性)
- [构建要求](#构建要求)
- [获取源码](#获取源码)
- [构建](#构建)
- [运行](#运行)
- [配置文件](#配置文件)
- [IP 地址来源](#ip-地址来源)
- [DNS 解析器](#dns-解析器)
- [驱动](#驱动)
- [TLS 与 CA 证书](#tls-与-ca-证书)
- [以服务方式运行](#以服务方式运行)
- [驱动插件 ABI](#驱动插件-abi)
- [编写自定义驱动](#编写自定义驱动)
- [升级到 v1.x](#升级到-v1x)
- [依赖组件](#依赖组件)
- [许可证](#许可证)

## 功能特性

- 通过一份 JSON 配置管理多个域名与多条记录，更新间隔按域名设置，并可配置周期性的
  强制更新。
- 地址来源包括本地网络接口与 HTTP(S) 端点。
- 服务商驱动在运行时以共享库形式加载；随附 Cloudflare、DigitalOcean、DNSPod 以及
  一个通用 HTTP 驱动。
- DNS 解析支持系统解析器、自定义 UDP 服务器、DNS-over-HTTPS 与 DNS-over-TLS。
- A 与 AAAA 记录相互独立；解析器同时支持 TXT 与 SOA 查询。
- 记录更新在线程池上并发执行。
- 收到 SIGINT/SIGTERM 后执行有序退出。

## 构建要求

| 组件 | 最低要求 |
|---|---|
| CMake | 3.14 |
| C++ 编译器 | 支持 C++17（建议 GCC 9+；GCC 7/8 通过自动链接 `stdc++fs` 获得支持） |
| OpenSSL | 1.1.1 或更高（同时支持 OpenSSL 3.x） |
| zlib | 任意较新版本 |

其余依赖均以 git 子模块形式随源码提供，见[依赖组件](#依赖组件)。

常见发行版的参考说明：

| 发行版 | 说明 |
|---|---|
| Ubuntu 20.04 及更高 | 使用系统软件包即可构建。 |
| Ubuntu 18.04 | 需另行安装 CMake 3.14+（例如通过 pip）；默认 GCC 7 依赖自动的 `stdc++fs` 回退。 |
| Debian 10 及更高 | 系统自带的 CMake 版本不足，需另行安装 3.14+。 |
| RHEL / CentOS / Rocky / Alma 8 及更高 | 需要 GCC 9 或更高（例如 `gcc-toolset-10`）；RHEL 7 附带 OpenSSL 1.0.2，不受支持。 |
| Alpine 3.11 及更高 | 自动检测 musl 并相应关闭 LTO。 |

## 获取源码

依赖以 git 子模块形式管理，须随源码一并获取：

```bash
# 克隆时同时获取子模块
git clone --recursive -b v0.x https://github.com/CyberKoo/yaddnsc.git

# 或先克隆再初始化子模块
git clone -b v0.x https://github.com/CyberKoo/yaddnsc.git
cd yaddnsc
git submodule update --init --recursive --depth 1
```

切换分支或拉取更新后，请刷新子模块：

```bash
git submodule update --recursive --depth 1
```

## 构建

Ubuntu 20.04 及更高版本：

```bash
sudo apt install build-essential cmake libssl-dev zlib1g-dev

mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Ubuntu 18.04（系统 CMake 版本过低）：

```bash
sudo apt install build-essential libssl-dev zlib1g-dev python3-pip git
pip install --user "cmake<3.24"

mkdir build && cd build
~/.local/bin/cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

构建产物为：可执行文件 `build/objs/yaddnsc`，驱动模块 `build/objs/driver/*.so`。
本分支不提供安装规则与软件包，请手动复制文件部署（见[以服务方式运行](#以服务方式运行)）。

### CMake 选项

| 选项 | 默认值 | 说明 |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release` | 调试构建时设为 `Debug`。 |
| `LIBC_MUSL` | 自动检测 | musl 相关调整（关闭 LTO）；可用 `-DLIBC_MUSL=ON/OFF` 覆盖。 |
| `NO_RTTI` | `OFF` | 以 `-fno-rtti` 构建，减小二进制体积。 |

## 运行

```bash
yaddnsc -c /etc/yaddnsc/config.json
```

| 选项 | 说明 |
|---|---|
| `-c, --config <路径>` | 配置文件路径；默认 `./config.json`。 |
| `-v, --verbose` | 输出调试级别日志。 |
| `-V, --version` | 输出版本号并退出。 |
| `-h, --help` | 输出用法说明并退出。 |

程序启动时加载配置与驱动，并在驱动加载完成后校验配置；配置非法时启动中止。发生
致命错误时，程序记录一条 critical 级别日志并以非零状态退出。

SIGINT 与 SIGTERM 触发有序退出：进行中的更新完成后进程才结束。重复发送 SIGINT 将
逐级升级为立即终止。

## 配置文件

yaddnsc 读取 JSON 配置文件，默认为 `./config.json`。完整示例见随附的
[`config.example.json`](config.example.json)：

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

| 字段 | 类型 | 说明 |
|---|---|---|
| `driver_dir` | string | 驱动模块所在目录；可选，默认为空（此时 `load` 中的路径相对于工作目录解析）。 |
| `load` | string[] | 待加载的驱动模块文件名，须包含 `.so` 后缀；必填。条目相对于 `driver_dir` 解析。 |

### `resolver`

| 字段 | 类型 | 说明 |
|---|---|---|
| `use_custom_server` | boolean | 必填。为 `false` 时使用系统解析器（`/etc/resolv.conf`），其余字段不生效。 |
| `ipaddress` | string | 即使 `use_custom_server` 为 `false` 也必须填写。纯 IP 表示传统 DNS，`https://` URL 表示 DoH，`tls://` 地址表示 DoT；详见 [DNS 解析器](#dns-解析器)。 |
| `port` | integer | 服务器端口；可选，默认 53。 |
| `protocol` | string | 仅为兼容保留，取值被忽略；协议始终根据 `ipaddress` 前缀判定。 |

### `domains[]`

以下字段均为必填。

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 域名，例如 `example.com`。 |
| `update_interval` | int | 更新间隔，单位秒；最小值 60。 |
| `force_update` | int | 强制更新间隔，单位秒；到达该间隔时不与 DNS 比对即推送更新。`0` 表示关闭；非零时不得小于 `update_interval`。 |
| `driver` | string | 处理该域名的驱动名称，须与已加载的驱动一致。 |
| `subdomains` | array | 该域名下受管理的记录。 |

### `subdomains[]`

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 记录标签，例如 `home.example.com` 对应 `home`；必填。 |
| `type` | string | 记录类型：`a`、`aaaa`、`txt` 或 `soa`（区分大小写）；必填。动态更新使用 `a` 或 `aaaa`。 |
| `interface` | string | 网络接口名称；该键必填，但可以为空字符串（`""`）。非空时作为接口来源的选取接口，并绑定出站 HTTP 流量；详见 [IP 地址来源](#ip-地址来源)。 |
| `ip_type` | string | `ipv4`、`ipv6` 或 `unspecified`（默认）。决定 HTTP 连接（`url` 来源请求与驱动 API 请求）使用的地址族。 |
| `ip_source` | string | `interface` 或 `url`；必填。详见 [IP 地址来源](#ip-地址来源)。 |
| `ip_source_param` | string | `ip_source` 为 `url` 时待查询的 HTTP(S) URL；`interface` 来源不使用该字段。该键必填。 |
| `allow_ula` | boolean | 允许接口来源采用 IPv6 唯一本地地址（fc00::/7），默认 `false`。 |
| `allow_local_link` | boolean | 允许接口来源采用 IPv6 链路本地（fe80::/10）与站点本地（fec0::/10）地址，默认 `false`。 |
| `driver_param` | object | 驱动专有参数；必填。键与值均须为字符串。 |

`type` 与 `ip_type` 两个字段名称相近但用途不同：`type` 决定记录类型，并在
`interface` 来源下决定本地采集的地址族；`ip_type` 决定 `url` 来源请求与驱动 API
请求所用连接的地址族。

每次更新前，主程序会向驱动参数注入 `domain`、`subdomain`、`ip_addr`、`rd_type`、
`fqdn` 五个键及其运行时取值；`driver_param` 中的同名显式条目优先。因此，将上述键
声明为必填的驱动（例如 DigitalOcean 的 `domain`）无需手动填写。

### 凭据保护

`driver_param` 通常包含 API 令牌或密钥。请勿将配置文件纳入版本控制，应限制其访问
权限（`chmod 600 config.json`），并且仅为每个凭据授予完成更新所需的最小权限。

## IP 地址来源

### `interface`

读取 `interface` 字段指定的本地接口的地址。A 记录取 IPv4 地址，AAAA 记录取 IPv6
地址。除非设置 `allow_ula`，唯一本地地址不参与选取；除非设置 `allow_local_link`，
链路本地与站点本地地址不参与选取。

### `url`

向 `ip_source_param` 发起 HTTP GET 请求，并将响应正文按纯文本 IP 地址解析。建议
使用 HTTPS 端点。`interface` 字段非空时，出站连接绑定到该接口；驱动的更新请求同样
应用此绑定。

## DNS 解析器

`use_custom_server` 为 `false` 时，查询经由系统解析器。为 `true` 时，`ipaddress`
的形式决定所用协议：

| 形式 | 协议 | 端口 |
|---|---|---|
| 纯 IPv4/IPv6 地址 | UDP 上的传统 DNS | `port`（默认 53） |
| `https://host[/path]` | DNS-over-HTTPS | 取自 URL（443）；`port` 字段被忽略 |
| `tls://host` | DNS-over-TLS | `port`（默认 53——请显式设为 853） |

各种形式的补充说明：

- 传统自定义服务器通过解析器库生效；在缺少 `res_nquery` 的平台上，自定义服务器被
  忽略并记录警告。IPv6 自定义服务器需要构建时检测到的平台支持。
- DoH 的路径省略时默认为 `/dns-query`。
- DoT 的 `tls://` 前缀会被移除，其余部分作为服务器主机名（兼作 TLS SNI）。端口
  不会自动推断，而是取自 `port` 字段，其默认值 53 并不适用——请设置
  `"port": 853`。

查询失败时以一秒间隔重试至多五次，仍失败则跳过该记录的本次更新。

## 驱动

随附四个驱动，构建产物分别为 `cloudflare.so`、`digital_ocean.so`、`dnspod.so`、
`simple.so`。域名通过 `driver` 字段按名称选用驱动，参数以字符串键值对形式置于记录
的 `driver_param` 中。必填键缺失时，错误在更新对应记录时报告。

`domain`、`subdomain`、`ip_addr`、`rd_type`、`fqdn` 五个键在更新时由主程序自动
注入（见[配置文件](#配置文件)），下表不再列入。

### Cloudflare（`cloudflare`）

通过 Cloudflare API v4 更新已存在的记录，凭 Bearer API 令牌认证。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `sub_domain` | 是 | — | 记录的完整名称，例如 `home.example.com` |
| `zone_id` | 是 | — | 域名的 Zone ID |
| `record_id` | 是 | — | 待更新记录的 ID |
| `token` | 是 | — | 持有该域名 DNS 编辑权限的 API 令牌 |
| `ttl` | 否 | `"30"` | TTL，单位秒 |
| `proxied` | 否 | `"0"` | 是否经 Cloudflare 代理转发；真值为 `1`、`on`、`true`、`yes` |

注意拼写 `sub_domain`（含下划线）：自动注入的 `subdomain` 键不能替代它，必须显式
填写。

### DigitalOcean（`digital_ocean`）

通过 DigitalOcean API v2 更新已存在的记录，凭个人访问令牌认证。更新仅修改记录
内容，不提供 TTL 设置。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `record_id` | 是 | — | 待更新记录的 ID |
| `token` | 是 | — | 个人访问令牌（Personal Access Token） |

### DNSPod（`dnspod`）

通过 DNSPod API（`Record.Ddns`）更新已存在的记录，凭登录令牌认证。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `domain_id` | 是 | — | 域名 ID |
| `record_id` | 是 | — | 记录 ID |
| `login_token` | 是 | — | API 令牌，格式为 `ID,Token` |
| `global` | 否 | `"false"` | 真值（`1`、`on`、`true`、`yes`）表示使用国际端点，否则使用国内端点 |
| `record_line` | 否 | `"默认"` | 记录线路名称 |
| `record_line_id` | 否 | `"0"` | 记录线路 ID |

### Simple（`simple`）

向配置的 URL 发起 HTTP GET 请求，适用于自定义更新端点。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `url` | 是 | — | 请求 URL |
| `format` | 否 | — | 该键存在时（取值不限），URL 按模板处理 |

`format` 存在时，URL 中的 `{name}` 占位符被替换为 `driver_param` 中键为 `{name}`
（含花括号）的条目的值。例如条目 `"{token}": "abc123"` 使 `{token}` 可在 URL 中
使用。无对应条目的占位符会导致本次更新中止。自动注入的运行时取值（`ip_addr` 等）
在本分支的模板中不可见；请将该驱动用于根据请求来源确定地址的端点，或升级到
v1.x——v1.x 的模板可以引用这些取值。

该驱动不检查响应内容：任何完成的请求均视为成功。本分支的 Cloudflare 驱动亦是
如此。

## TLS 与 CA 证书

全部 HTTPS 流量——驱动 API 调用、`url` 来源请求以及 DoH/DoT 解析器——均依据 CA
证书包校验。证书包按以下顺序查找：工作目录下的 `./ca.pem`，然后是若干平台相关的
系统路径（Debian/Ubuntu、RHEL/Fedora、openSUSE、macOS Homebrew 等）。

本分支不支持 `SSL_CERT_FILE`。使用私有 CA 时，请将 PEM 证书包放至可执行文件旁的
`./ca.pem`，或安装到系统位置。若任何位置都不存在证书包，证书校验将被关闭并记录
一条提示；生产环境请确保证书包可用。

## 以服务方式运行

仓库根目录提供示例 systemd 单元 [`yaddnsc.service`](yaddnsc.service)。手动安装
示例：

```bash
sudo install -D build/objs/yaddnsc /opt/yaddnsc/yaddnsc
sudo install -D -t /opt/yaddnsc/drivers build/objs/driver/*.so
sudo install -D -m 600 config.json /etc/yaddnsc/config.json
sudo install -D yaddnsc.service /etc/systemd/system/yaddnsc.service

sudo systemctl daemon-reload
sudo systemctl enable --now yaddnsc
journalctl -u yaddnsc
```

该单元以 `nobody` 用户启动 `/opt/yaddnsc/yaddnsc -c /etc/yaddnsc/config.json`，
并在进程退出后自动重启。请根据实际部署调整路径与服务用户；配置文件须对服务账户
可读。

## 驱动插件 ABI

驱动为启动时经 `dlopen` 加载的共享库。每个驱动上报的 ABI 版本须与宿主完全一致；
本分支的 ABI 版本为 `1000000`，在 v0.x 各版本间保持不变，既有 v0.x `.so` 文件无需
重新编译即可继续使用。基于 v1.x SDK 构建的驱动采用不同的 ABI，加载时会被拒绝。

## 编写自定义驱动

驱动实现 `IDriver` 接口（`include/IDriver.h`），构建为无 `lib` 前缀的 `MODULE`
库，并导出两个 C 符号：

```cpp
extern "C" IDriver *create();          // 工厂函数
extern "C" void destroy(IDriver *);    // 销毁函数
```

接口组成：

- `generate_request(config)`——接收参数表（含自动注入的运行时键），返回描述更新
  请求 URL、方法、请求头与正文的 `driver_request`。
- `check_response(body)`——检查响应正文，报告更新是否成功。
- `get_detail()`——返回驱动名称、描述、作者与版本；配置中的 `driver` 字段即引用
  此处的名称。
- `get_driver_version()` 与 `init_logger()`——由 `BaseDriver`
  （`driver/base_driver.h`）提供，请勿覆盖。

`BaseDriver` 另提供：`check_required_params()` 用于必填键校验（缺失时在更新时抛出
异常）、`get_optional()` 用于读取可选参数，以及基于命名或位置参数的 `vformat()`
字符串替换辅助函数。`driver/` 目录下的随附驱动可作为完整示例。

## 升级到 v1.x

`master` 分支（v1.x）为完全重写，采用不同的驱动 ABI；v0.x 插件无法直接使用，须
基于新 SDK 重新构建。v1.x 需要较新的工具链（CMake 3.28+、GCC 14+、Clang 19+ 或
Apple Clang 15+），并新增 mDNS 地址来源、基于子命令的 CLI（含配置校验与诊断）、
多服务器解析策略、安装规则与 DEB 打包、容器镜像等。两个分支的配置文件不完全
兼容，迁移时请参阅 v1.x 文档。

## 依赖组件

| 组件 | 版本 | 用途 |
|---|---|---|
| [spdlog](https://github.com/gabime/spdlog) | 1.13.0 | 日志 |
| [fmt](https://github.com/fmtlib/fmt) | 10.2.1 | 字符串格式化 |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.14.3 | HTTP 客户端 |
| [nlohmann_json](https://github.com/nlohmann/json) | 3.11.3 | JSON 解析 |
| [cxxopts](https://github.com/jarro2783/cxxopts) | 3.2.1 | 命令行解析 |
| [BS::thread_pool](https://github.com/bshoshany/thread-pool) | 4.1.0 | 线程池 |
| OpenSSL | 系统 | TLS |
| zlib | 系统 | 压缩 |

前六个组件为 `deps/` 目录下的 git 子模块，随项目一并构建。

## 许可证

本项目依据 MIT 许可证发布，详见 [LICENSE](LICENSE)。
