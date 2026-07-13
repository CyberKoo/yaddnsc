# 驱动

**驱动（Driver）** 是一个运行时动态加载的插件（`.so` 共享库），负责将 DNS 更新
请求转换为特定服务商 API 所期望的 HTTP 调用。每个驱动承担三项职责：

1. **请求构造** — 根据目标 IP 地址和记录元数据，组装正确的 URL、请求头、
   请求体和 HTTP 方法。
2. **响应验证** — 解析服务商的响应，判断成功或失败，并在出错时记录详细的
   错误信息。
3. **自我描述** — 每个驱动暴露其名称、版本、作者和描述信息，使应用能在启动时
   列出已加载的插件并验证 ABI 兼容性。

驱动在运行时通过 `dlopen(3)` 按需加载——添加、移除或更新驱动无需重新编译
主程序，只要插件的 ABI 保持兼容即可。

## 参数配置

每个驱动通过配置中 `driver_param` 字段下的 JSON 子对象来接收其特有参数。
可用的键和值类型因驱动而异；下文列出了每个随附驱动的完整参数说明。

内部实现上，JSON 数据通过 **glaze**（编译期反射，零运行时开销）反序列化为
**强类型的 C++ 结构体**。如果必填参数缺失或类型错误，应用会精确报告出错的
字段并退出，附带清晰的诊断信息——不会发生静默配置错误。驱动不识别的额外键
会被静默忽略，因此多个配置可以共享 `driver_param` 块而不会报错。

**示例（Cloudflare）：**
```json
{
  "driver_param": {
    "zone_id": "abc123",
    "record_id": "xyz789",
    "token": "your-api-token",
    "ttl": 120
  }
}
```

以下是所有随附驱动的完整参数参考。

- [Alibaba Cloud](#alibaba-cloudalibaba_cloudso)
- [Cloudflare](#cloudflarecloudflareso)
- [DigitalOcean](#digitaloceandigital_oceanso)
- [DNSPod](#dnspoddnspodso)
- [DuckDNS](#duckdnsduckdnsso)
- [GoDaddy](#godaddygodaddyso)
- [Linode](#linodelinodeso)
- [Namecheap](#namecheapnamecheapso)
- [Porkbun](#porkbunporkbunso)
- [Route 53](#route-53route53so)
- [Simple](#simplesimpleso)
- [Vultr](#vultrvultrso)

---

## Alibaba Cloud（`alibaba_cloud.so`）

通过 [Alibaba Cloud DNS UpdateDomainRecord API](https://www.alibabacloud.com/help/en/dns/api-alidns-2015-01-09-updatedomainrecord) 更新 DNS 记录，使用阿里云 RPC 签名（HMAC-SHA1）。

- 支持 **A（IPv4）** 和 **AAAA（IPv6）** 记录。

| 参数 | 必需 | 说明 |
|------|------|------|
| `access_key_id` | 是 | 阿里云 AccessKey ID |
| `access_key_secret` | 是 | 阿里云 AccessKey Secret |
| `record_id` | 是 | DNS 记录 ID |
| `ttl` | 否 | TTL，单位秒（默认：600） |

**注意：** 仅支持国际站端点，不支持国内站。

## Cloudflare（`cloudflare.so`）

通过 [Cloudflare API v4](https://developers.cloudflare.com/api/) 更新 DNS 记录。

- 支持 **A（IPv4）** 和 **AAAA（IPv6）** 记录。

| 参数          | 必需 | 说明                                   |
|-------------|----|--------------------------------------|
| `zone_id`   | 是  | Cloudflare Zone ID                   |
| `record_id` | 是  | Cloudflare DNS 记录 ID                 |
| `token`     | 是  | Cloudflare API Token（需要 DNS:Edit 权限） |
| `proxied`   | 否  | 是否通过 Cloudflare 代理（CDN）              |
| `ttl`       | 否  | TTL，单位秒（默认 30）                       |

**注意：** API Token 需要对域名具有 `DNS:Edit` 权限。TTL 设为 `30` 表示启用 Cloudflare 自动 TTL 模式。

## DigitalOcean（`digital_ocean.so`）

通过 [DigitalOcean API v2](https://developers.digitalocean.com/documentation/v2/) 更新 DNS 记录。

- 支持 **A（IPv4）** 和 **AAAA（IPv6）** 记录（更新已有记录，无法更改记录类型）。

| 参数          | 必需 | 说明                                 |
|-------------|----|------------------------------------|
| `record_id` | 是  | DigitalOcean DNS 记录 ID             |
| `token`     | 是  | DigitalOcean Personal Access Token |

**注意：** 不支持 TTL 配置。

## DNSPod（`dnspod.so`）

通过 [DNSPod API](https://www.dnspod.com/docs/) 更新 DNS 记录，同时支持国内和国际端点。

- 支持 **A（IPv4）** 和 **AAAA（IPv6）** 记录。

| 参数               | 必需 | 说明                                   |
|------------------|----|--------------------------------------|
| `domain_id`      | 是  | DNSPod 域名 ID                         |
| `record_id`      | 是  | DNSPod 记录 ID                         |
| `login_token`    | 是  | DNSPod API 登录令牌（ID,Token 格式）         |
| `global`         | 否  | 使用国际 API 端点（`true`）或国内端点（`false`，默认） |
| `record_line`    | 否  | 记录线路（国内端点默认 `"默认"`，国际端点默认 `"default"`） |
| `record_line_id` | 否  | 记录线路 ID（默认：`"0"`）                    |

**注意：** `login_token` 必须使用 `ID,Token` 格式（逗号分隔）。API 有频率限制（错误码 `-2`）。

## DuckDNS（`duckdns.so`）

基于简单 GET 请求的 [DuckDNS](https://www.duckdns.org/) 免费动态 DNS 服务驱动。通过单个 HTTPS GET 请求更新 A 和 AAAA 记录。

- 支持 **A（IPv4）** 和 **AAAA（IPv6）** 记录。

| 参数 | 必需 | 说明 |
|------|------|------|
| `token` | 是 | DuckDNS API Token |
| `verbose` | 否 | 启用详细响应解析以获取更详细的日志信息（默认：false） |

**注意：** 该驱动仅适用于 DuckDNS 服务（`*.duckdns.org` 域名）。不支持 TTL 配置。

## GoDaddy（`godaddy.so`）

通过 [GoDaddy Domains API v1](https://developer.godaddy.com/doc/endpoint/domains) 更新 DNS 记录，使用 SSO key 认证。

- 支持 **A（IPv4）** 和 **AAAA（IPv6）** 记录。

| 参数 | 必需 | 说明 |
|------|------|------|
| `key` | 是 | GoDaddy API key（SSO key 前缀） |
| `secret` | 是 | GoDaddy API secret（SSO key 后缀） |
| `ttl` | 否 | DNS 记录 TTL，单位秒（默认：600） |

**注意：** 认证使用 SSO key 格式 `key:secret`。更新成功返回 HTTP 200，响应体为空。

## Linode（`linode.so`）

通过 [Linode API v4](https://techdocs.akamai.com/linode-api/reference/put-domain-record) 更新 DNS 记录，使用 Personal Access Token 认证。

- 支持 **A（IPv4）** 和 **AAAA（IPv6）** 记录（更新已有记录，无法更改记录类型）。

| 参数 | 必需 | 说明 |
|------|------|------|
| `token` | 是 | Linode Personal Access Token |
| `domain_id` | 是 | Linode 域名 ID |
| `record_id` | 是 | DNS 记录 ID |
| `ttl_sec` | 否 | TTL，单位秒（可选值：300, 3600, 7200, 14400, 28800, 57600, 86400, 172800, 345600, 604800, 1209600, 2419200，其他值会被四舍五入） |

**注意：** TTL 仅接受表中列出的特定值，其他值会被四舍五入到最近的合法值。

## Namecheap（`namecheap.so`）

通过 [Namecheap Dynamic DNS API](https://www.namecheap.com/support/knowledgebase/article.aspx/29/11/how-to-configure-your-dns-dynamic-dns-update-url/) 更新 DNS 记录，使用基于 GET 的更新端点。

- **仅支持 A（IPv4）记录**。上游 API 不支持 AAAA（IPv6）记录。

构建时需要 **libxml2**。如果系统未安装 libxml2，CMake 会打印警告并跳过此驱动。

| 参数       | 必需 | 说明                                      |
|-----------|------|-----------------------------------------|
| `password` | 是   | Namecheap Dynamic DNS 密码（Advanced DNS → Dynamic DNS 中获取，不是账户密码） |

## Porkbun（`porkbun.so`）

通过 [Porkbun API v3](https://porkbun.com/api/json/v3/documentation) 更新 DNS 记录，使用 API key + Secret key 认证。

- 支持 **A（IPv4）** 和 **AAAA（IPv6）** 记录。

| 参数 | 必需 | 说明 |
|------|------|------|
| `api_key` | 是 | Porkbun API key |
| `secret_api_key` | 是 | Porkbun Secret API key |
| `ttl` | 否 | TTL，单位秒（默认：账户最小值） |

**注意：** 该驱动使用 `editByNameType` 端点。如果同一子域名下存在多条相同类型的记录，更新行为未定义。

## Route 53（`route53.so`）

通过 [AWS Route 53 ChangeResourceRecordSets API](https://docs.aws.amazon.com/Route53/latest/APIReference/API_ChangeResourceRecordSets.html) 更新 DNS 记录，使用 SigV4 请求签名认证。

- 支持 **A（IPv4）** 和 **AAAA（IPv6）** 记录。

构建时需要 **libxml2**。如果系统未安装 libxml2，CMake 会打印警告并跳过此驱动。

| 参数 | 必需 | 说明 |
|------|------|------|
| `access_key_id` | 是 | AWS 访问密钥 ID |
| `secret_access_key` | 是 | AWS 秘密访问密钥 |
| `hosted_zone_id` | 是 | Route 53 托管区 ID（例如 `Z3M79L5CQABCDE`） |
| `region` | 是 | AWS 区域（例如 `us-east-1`） |
| `record_name` | 是 | DNS 记录名称（要更新的子域名） |
| `ttl` | 否 | TTL，单位秒（默认：300） |

**注意：** 该驱动使用 UPSERT 操作——如记录不存在则自动创建。FQDN 末尾的点会自动补全。

## Simple（`simple.so`）

通用 HTTP GET 驱动，适用于自定义 API。将 `url` 视为模板，将 `{key}` 占位符替换为配置中的值和运行时上下文的值。

| 参数    | 必需 | 说明                                                                   |
|-------|----|----------------------------------------------------------------------|
| `url` | 是  | HTTP(S) URL 模板，支持 `{key}` 占位符。`driver_param` 中的其他键都会作为 `{key}` 参与替换。 |

**可用的替换变量：**

| 变量            | 来源             | 说明                              |
|---------------|----------------|---------------------------------|
| `{ip_addr}`   | 运行时            | 检测到的 IP 地址                      |
| `{rd_type}`   | 运行时            | DNS 记录类型（A、AAAA）                |
| `{domain}`    | 运行时            | 域名                              |
| `{subdomain}` | 运行时            | 子域名名称                           |
| `{fqdn}`      | 运行时            | 完整域名                            |
| `{any_key}`   | `driver_param` | `driver_param` 中的任意键（除 `url` 外） |

示例：
```json
{
  "driver_param": {
    "url": "https://api.example.com/update?ip={ip_addr}&type={rd_type}&domain={domain}",
    "key": "my-secret-key"
  }
}
```

只要响应的 body 非空即视为成功。

## Vultr（`vultr.so`）

通过 [Vultr API v2](https://www.vultr.com/api/#tag/dns) 更新 DNS 记录，使用 Bearer token 认证。

- 支持 **A（IPv4）** 和 **AAAA（IPv6）** 记录（更新已有记录，无法更改记录类型）。

| 参数 | 必需 | 说明 |
|------|------|------|
| `api_key` | 是 | Vultr API key（Bearer token） |
| `record_id` | 是 | DNS 记录 ID |
| `ttl` | 否 | TTL，单位秒（默认：服务器默认值） |

**注意：** 该驱动使用 HTTP PATCH 方法（部分代理对 PATCH 支持有限）。更新成功返回 HTTP 204 No Content。
