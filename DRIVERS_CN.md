# DNS 服务商驱动

yaddnsc 随附十二个以可加载模块形式提供的服务商驱动，本文档是各驱动的参数参考。
安装与通用配置请参阅 [README_CN.md](README_CN.md)；开发新的驱动请参阅
[docs/custom-drivers.md](docs/custom-drivers.md)。

## 通用规则

- 域名通过 `driver` 字段选用驱动，取值为下表中的配置名称。
- 参数置于 `driver_param` 中，该字段仅在子域名（subdomain）条目内有效。每条记录
  各自携带完整的 `driver_param`，不存在域名层级的继承。
- 每个驱动都会在 `yaddnsc config test` 期间校验其 `driver_param`：缺少必填键或
  出现无法识别的键均属于校验错误。`simple` 驱动例外，允许任意附加键。
- 所有驱动均更新已存在的记录，请先在服务商处创建记录后再在配置中引用。唯一例外
  是 `route53`，记录不存在时会自动创建。
- 没有任何随附驱动支持更新 TXT 记录；`txt` 类型仅对诊断命令
  `yaddnsc dns resolve` 有意义。
- 请勿将凭据纳入版本控制；应限制配置文件权限（例如 `chmod 600 config.json`），
  并且仅为每个凭据授予完成更新所需的最小权限。

| 配置名称 | 模块文件 |
|---|---|
| `alibaba_cloud` | `alibaba_cloud.so` |
| `cloudflare` | `cloudflare.so` |
| `digital_ocean` | `digital_ocean.so` |
| `dnspod` | `dnspod.so` |
| `duckdns` | `duckdns.so` |
| `godaddy` | `godaddy.so` |
| `linode` | `linode.so` |
| `namecheap` | `namecheap.so` |
| `porkbun` | `porkbun.so` |
| `route53` | `route53.so` |
| `simple` | `simple.so` |
| `vultr` | `vultr.so` |

## 能力概览

| 服务商 | 驱动 | A | AAAA | 记录处理方式 |
|---|---|---:|---:|---|
| Alibaba Cloud | `alibaba_cloud` | 是 | 是 | 更新已存在的记录 |
| Cloudflare | `cloudflare` | 是 | 是 | 更新已存在的记录 |
| DigitalOcean | `digital_ocean` | 是 | 是 | 更新已存在的记录 |
| DNSPod | `dnspod` | 是 | 是 | 更新已存在的记录 |
| DuckDNS | `duckdns` | 是 | 是 | 仅限 DuckDNS 注册的名称 |
| GoDaddy | `godaddy` | 是 | 是 | 替换指定名称与类型的记录集 |
| Linode | `linode` | 是 | 是 | 更新已存在的记录 |
| Namecheap | `namecheap` | 是 | — | 更新已存在的记录 |
| Porkbun | `porkbun` | 是 | 是 | 按名称与类型定位记录 |
| Route 53 | `route53` | 是 | 是 | 记录不存在时创建 |
| Simple | `simple` | 是 | 是 | 通用 HTTP 端点 |
| Vultr | `vultr` | 是 | 是 | 更新已存在的记录 |

除本文所述行为外，服务商侧的约束（账户套餐、频率限制、API 配额等）同样适用。

目录：

- [Alibaba Cloud](#alibaba-cloudalibaba_cloud)
- [Cloudflare](#cloudflarecloudflare)
- [DigitalOcean](#digitaloceandigital_ocean)
- [DNSPod](#dnspoddnspod)
- [DuckDNS](#duckdnsduckdns)
- [GoDaddy](#godaddygodaddy)
- [Linode](#linodelinode)
- [Namecheap](#namecheapnamecheap)
- [Porkbun](#porkbunporkbun)
- [Route 53](#route-53route53)
- [Simple](#simplesimple)
- [Vultr](#vultrvultr)

---

## Alibaba Cloud（`alibaba_cloud`）

记录类型：A、AAAA（更新已存在的记录）。使用
[Alibaba Cloud DNS API](https://www.alibabacloud.com/help/en/dns/api-alidns-2015-01-09-updatedomainrecord)，
请求经 AccessKey 签名。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `access_key_id` | 是 | — | 阿里云 AccessKey ID |
| `access_key_secret` | 是 | — | 阿里云 AccessKey Secret |
| `record_id` | 是 | — | 待更新记录的 ID |
| `ttl` | 否 | 600 | TTL，单位秒 |

仅支持国际站端点，不支持中国站端点。

## Cloudflare（`cloudflare`）

记录类型：A、AAAA（更新已存在的记录）。使用
[Cloudflare API v4](https://developers.cloudflare.com/api/)，凭 API 令牌认证。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `zone_id` | 是 | — | 域名的 Zone ID |
| `record_id` | 是 | — | 待更新记录的 ID |
| `token` | 是 | — | API 令牌 |
| `ttl` | 否 | 30 | TTL，单位秒 |
| `proxied` | 否 | `false` | 是否经 Cloudflare 代理转发 |

令牌须持有对应域名的 DNS 编辑权限。Zone ID 与记录 ID 可在 Cloudflare 控制台或
API 中查询。

## DigitalOcean（`digital_ocean`）

记录类型：A、AAAA（更新已存在的记录）。使用
[DigitalOcean API v2](https://developers.digitalocean.com/documentation/v2/)，
凭个人访问令牌认证。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `record_id` | 是 | — | 待更新记录的 ID |
| `token` | 是 | — | 个人访问令牌（Personal Access Token） |

更新仅修改记录内容；该驱动不提供 TTL 设置。

## DNSPod（`dnspod`）

记录类型：A、AAAA（更新已存在的记录）。使用
[DNSPod API](https://www.dnspod.com/docs/)（`Record.Ddns`），凭登录令牌认证。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `domain_id` | 是 | — | 域名 ID |
| `record_id` | 是 | — | 记录 ID |
| `login_token` | 是 | — | API 令牌，格式为 `ID,Token` |
| `global` | 是 | — | `false` 使用国内端点，`true` 使用国际端点 |
| `record_line_id` | 是 | — | 记录线路 ID；默认线路填 `"0"` |
| `record_line` | 否 | `"默认"`（国内）/ `"default"`（国际） | 记录线路名称 |

`global` 与 `record_line_id` 虽属于固定取值类设置，但驱动将其视为必填键，省略
任一均会导致校验失败。`login_token` 必须是以逗号分隔的 `ID,Token` 形式。该 API
设有频率限制，返回错误码 `-2` 表示请求过于频繁。

## DuckDNS（`duckdns`）

记录类型：A、AAAA。使用 [DuckDNS](https://www.duckdns.org/) 更新端点，凭账户令牌
认证。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `token` | 是 | — | DuckDNS 站点提供的账户令牌 |
| `verbose` | 否 | `false` | 请求详细响应，用于输出更多日志 |

该驱动仅适用于在 DuckDNS 注册的名称：所配置记录的子域名标签须为 DuckDNS 主机名。
不支持 TTL 设置。

## GoDaddy（`godaddy`）

记录类型：A、AAAA。使用
[GoDaddy Domains API v1](https://developer.godaddy.com/doc/endpoint/domains)，
凭密钥对认证。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `key` | 是 | — | API 密钥（key） |
| `secret` | 是 | — | API 密钥（secret） |
| `ttl` | 否 | 600 | TTL，单位秒 |

每次更新会替换指定名称与类型的整个记录集。

## Linode（`linode`）

记录类型：A、AAAA（更新已存在的记录）。使用
[Linode API v4](https://techdocs.akamai.com/linode-api/reference/put-domain-record)，
凭个人访问令牌认证。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `token` | 是 | — | 个人访问令牌（Personal Access Token） |
| `domain_id` | 是 | — | 域名 ID |
| `record_id` | 是 | — | 待更新记录的 ID |
| `ttl_sec` | 否 | 保持不变 | TTL，单位秒；未设置时不随请求提交 |

Linode API 仅接受一组固定的 TTL 取值，其余取值会被拒绝；非法的 `ttl_sec` 将在
更新时表现为 API 错误。

## Namecheap（`namecheap`）

记录类型：仅 A。使用
[Namecheap Dynamic DNS API](https://www.namecheap.com/support/knowledgebase/article.aspx/29/11/how-to-configure-your-dns-dynamic-dns-update-url/)。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `password` | 是 | — | Namecheap 控制台（Advanced DNS）中的 Dynamic DNS 密码，并非账户密码 |

上游 API 不支持 AAAA 记录，AAAA 更新请求会在发出前被拒绝。该驱动仅在构建时系统
提供 libxml2 的情况下编译。

## Porkbun（`porkbun`）

记录类型：A、AAAA。使用
[Porkbun API v3](https://porkbun.com/api/json/v3/documentation)，凭 API 密钥对
认证。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `api_key` | 是 | — | API 密钥 |
| `secret_api_key` | 是 | — | Secret API 密钥 |
| `ttl` | 否 | 账户最低值 | TTL，单位秒；未设置时不随请求提交 |

记录按名称与类型定位。若同一名称与类型下存在多条记录，被更新的记录无法确定；
请保持每个名称与类型组合至多一条记录。

## Route 53（`route53`）

记录类型：A、AAAA。使用
[AWS Route 53 API](https://docs.aws.amazon.com/Route53/latest/APIReference/API_ChangeResourceRecordSets.html)，
请求经 SigV4 签名。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `access_key_id` | 是 | — | AWS 访问密钥 ID |
| `secret_access_key` | 是 | — | AWS 秘密访问密钥 |
| `hosted_zone_id` | 是 | — | 托管区（Hosted Zone）ID |
| `region` | 是 | — | 用于请求签名的 AWS 区域 |
| `record_name` | 是 | — | 记录名称；见下方说明 |
| `ttl` | 否 | 300 | TTL，单位秒 |

驱动执行 UPSERT 操作：记录不存在时自动创建。实际操作的记录始终由配置中的域名与
子域名组合成的完整名称决定；`record_name` 必须存在才能通过校验，但其取值不被
使用，建议将其填为同一名称以免误解。该驱动仅在构建时系统提供 libxml2 的情况下
编译。

## Simple（`simple`）

记录类型：A、AAAA。面向自定义 HTTP API 的通用驱动：向由模板构造的 URL 发起 GET
请求。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `url` | 是 | — | 含有 `{key}` 占位符的 HTTP(S) URL 模板 |

`driver_param` 中其余字符串类型的键均可作为 `{key}` 形式的替换变量使用。以下
内置变量始终可用：

| 变量 | 取值 |
|---|---|
| `{ip_addr}` | 检测到的 IP 地址 |
| `{rd_type}` | 记录类型（`A`、`AAAA`） |
| `{domain}` | 域名 |
| `{subdomain}` | 子域名标签 |
| `{fqdn}` | 记录的完整名称 |

示例：

```json
{
  "driver_param": {
    "url": "https://api.example.com/update?ip={ip_addr}&type={rd_type}&name={fqdn}",
    "key": "my-secret-key"
  }
}
```

端点返回小于 300 的 HTTP 状态码且响应正文非空时，更新视为成功。请求方法固定为
GET，且无法配置请求头；若 API 提供其他更安全的认证机制，应优先采用，而非将长期
有效的凭据嵌入 URL。

## Vultr（`vultr`）

记录类型：A、AAAA（更新已存在的记录）。使用
[Vultr API v2](https://www.vultr.com/api/#tag/dns)，凭 API 密钥认证。

| 参数 | 必填 | 默认值 | 说明 |
|---|---|---|---|
| `api_key` | 是 | — | API 密钥 |
| `record_id` | 是 | — | 待更新记录的 ID |
| `ttl` | 否 | 服务器默认值 | TTL，单位秒；未设置时不随请求提交 |

更新以 HTTP PATCH 请求发出，部分代理不支持转发该方法；更新成功时返回 HTTP 204。
