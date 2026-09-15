# DNS Provider Drivers

This reference lists the bundled DNS provider drivers and the parameters they
require. Select a provider, prepare its credentials, and copy its
`driver_param` example into your configuration.

For installation and general configuration, see [`README.md`](README.md). For
custom driver development, see [`docs/custom-drivers.md`](docs/custom-drivers.md).

## Before You Start

- Keep API tokens, keys, and passwords out of source control.
- Grant only the permissions required to update the intended DNS records.
- Restrict your configuration file, for example: `chmod 600 config.json`.
- Validate it before starting: `yaddnsc config test`.

| Configuration name | Library file |
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

## Support at a Glance

Use this table to narrow the provider list before reading its parameter section.
“Existing only” means the driver updates an existing record and does not create
one. Provider APIs and account plans can impose additional limits.

| Provider | Driver | A | AAAA | TXT | Record behaviour |
|---|---|---:|---:|---:|---|
| Alibaba Cloud | `alibaba_cloud` | Yes | Yes | — | Existing record |
| Cloudflare | `cloudflare` | Yes | Yes | Yes | Existing record |
| DigitalOcean | `digital_ocean` | Yes | Yes | — | Existing only |
| DNSPod | `dnspod` | Yes | Yes | — | Existing record |
| DuckDNS | `duckdns` | Yes | Yes | — | DuckDNS subdomain |
| GoDaddy | `godaddy` | Yes | Yes | — | Existing record |
| Linode | `linode` | Yes | Yes | — | Existing only |
| Namecheap | `namecheap` | Yes | — | — | Existing record |
| Porkbun | `porkbun` | Yes | Yes | — | Name and type lookup |
| Route 53 | `route53` | Yes | Yes | — | Creates or updates |
| Simple | `simple` | API-dependent | API-dependent | API-dependent | Custom HTTP API |
| Vultr | `vultr` | Yes | Yes | — | Existing only |

## Parameters

Place provider-specific settings in the `driver_param` object of the domain or
subdomain configuration. Required fields must be present and have the stated
type; unknown fields are ignored.

**Example — Cloudflare:**
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

Below is the complete reference of every bundled driver and the parameters it accepts.

- [Alibaba Cloud](#alibaba-cloud-alibaba_cloudso)
- [Cloudflare](#cloudflare-cloudflareso)
- [DigitalOcean](#digitalocean-digital_oceanso)
- [DNSPod](#dnspod-dnspodso)
- [DuckDNS](#duckdns-duckdnsso)
- [GoDaddy](#godaddy-godaddyso)
- [Linode](#linode-linodeso)
- [Namecheap](#namecheap-namecheapso)
- [Porkbun](#porkbun-porkbunso)
- [Route 53](#route-53-route53so)
- [Simple](#simple-simpleso)
- [Vultr](#vultr-vultrso)

---

## Alibaba Cloud (`alibaba_cloud.so`)

- Configuration name: `alibaba_cloud`
- Supported records: A, AAAA

Updates DNS records via the [Alibaba Cloud DNS UpdateDomainRecord API](https://www.alibabacloud.com/help/en/dns/api-alidns-2015-01-09-updatedomainrecord) using Alibaba Cloud RPC signature signing (HMAC-SHA1).

- Supports **A (IPv4)** and **AAAA (IPv6)** records.

| Parameter | Required | Description                                                    |
|-----------|----------|----------------------------------------------------------------|
| `access_key_id` | Yes | Alibaba Cloud AccessKey ID                              |
| `access_key_secret` | Yes | Alibaba Cloud AccessKey Secret                         |
| `record_id` | Yes     | DNS Record ID to update                                        |
| `ttl`      | No       | TTL in seconds (default: 600)                                  |

**Note:** International endpoint only. China endpoint is not supported.

## Cloudflare (`cloudflare.so`)

- Configuration name: `cloudflare`
- Supported records: A, AAAA, TXT

Updates DNS records via the [Cloudflare API v4](https://developers.cloudflare.com/api/).

- Supports **A (IPv4)**, **AAAA (IPv6)**, and **TXT** records.

| Parameter | Required | Description                                      |
|-----------|----------|--------------------------------------------------|
| `zone_id` | Yes      | Cloudflare Zone ID                               |
| `record_id` | Yes    | Cloudflare DNS Record ID                         |
| `token`   | Yes      | Cloudflare API Token (needs DNS:Edit permission) |
| `proxied` | No       | Whether the record is proxied through Cloudflare |
| `ttl`     | No       | TTL in seconds (default: 30)                     |

**Note:** The API token requires the `DNS:Edit` permission on the zone. A TTL of `30` enables Cloudflare's automatic TTL mode.

## DigitalOcean (`digital_ocean.so`)

- Configuration name: `digital_ocean`
- Supported records: A, AAAA

Updates DNS records via the [DigitalOcean API v2](https://developers.digitalocean.com/documentation/v2/).

- Supports **A (IPv4)** and **AAAA (IPv6)** records (updates existing records; cannot change record type).

| Parameter   | Required | Description                          |
|-------------|----------|--------------------------------------|
| `record_id` | Yes      | DigitalOcean DNS Record ID           |
| `token`     | Yes      | DigitalOcean Personal Access Token   |

**Note:** No TTL configuration is exposed.

## DNSPod (`dnspod.so`)

- Configuration name: `dnspod`
- Supported records: A, AAAA

Updates DNS records via the [DNSPod API](https://www.dnspod.com/docs/). Supports both China and Global endpoints.

- Supports **A (IPv4)** and **AAAA (IPv6)** records.

| Parameter        | Required | Description                                                           |
|------------------|----------|-----------------------------------------------------------------------|
| `domain_id`      | Yes      | DNSPod Domain ID                                                      |
| `record_id`      | Yes      | DNSPod Record ID                                                      |
| `login_token`    | Yes      | DNSPod API login token (ID,Token format)                              |
| `global`         | No       | Use global API endpoint (`true`) or China endpoint (`false`, default) |
| `record_line`    | No       | Record line (e.g. `"默认"` for default, `"default"` for global)         |
| `record_line_id` | No       | Record line ID (default: `"0"`)                                       |

**Note:** The `login_token` must be in `ID,Token` format (comma-separated). The API enforces rate limiting (error code `-2`).

## DuckDNS (`duckdns.so`)

- Configuration name: `duckdns`
- Supported records: A, AAAA

A simple GET-based driver for the [DuckDNS](https://www.duckdns.org/) free dynamic DNS service. Updates A and AAAA records via a single HTTPS GET request.

- Supports **A (IPv4)** and **AAAA (IPv6)** records.

| Parameter | Required | Description                                                                  |
|-----------|----------|------------------------------------------------------------------------------|
| `token`   | Yes      | DuckDNS API token                                                            |
| `verbose` | No       | Enable verbose response parsing for extra logging information (default: false) |

**Note:** This driver is specific to the DuckDNS service (`*.duckdns.org` domains). No TTL configuration is available.

## GoDaddy (`godaddy.so`)

- Configuration name: `godaddy`
- Supported records: A, AAAA

Updates DNS records via the [GoDaddy Domains API v1](https://developer.godaddy.com/doc/endpoint/domains) using SSO key authentication.

- Supports **A (IPv4)** and **AAAA (IPv6)** records.

| Parameter | Required | Description                                            |
|-----------|----------|--------------------------------------------------------|
| `key`     | Yes      | GoDaddy API key (SSO key prefix)                       |
| `secret`  | Yes      | GoDaddy API secret (SSO key suffix)                    |
| `ttl`     | No       | DNS record TTL in seconds (default: 600)               |

**Note:** Authentication uses SSO key format `key:secret`. A successful update returns HTTP 200 with an empty body.

## Linode (`linode.so`)

- Configuration name: `linode`
- Supported records: A, AAAA

Updates DNS records via the [Linode API v4](https://techdocs.akamai.com/linode-api/reference/put-domain-record) using Personal Access Token authentication.

- Supports **A (IPv4)** and **AAAA (IPv6)** records (updates existing records; cannot change record type).

| Parameter   | Required | Description                                                                                                                      |
|-------------|----------|----------------------------------------------------------------------------------------------------------------------------------|
| `token`     | Yes      | Linode Personal Access Token                                                                                                     |
| `domain_id` | Yes      | Linode Domain ID                                                                                                                 |
| `record_id` | Yes      | DNS Record ID to update                                                                                                          |
| `ttl_sec`   | No       | TTL in seconds. The Linode API only accepts specific values (300, 3600, 7200, 14400, 28800, 57600, 86400, 172800, 345600, 604800, 1209600, 2419200); invalid values are rejected by the API |

**Note:** The Linode API accepts only specific TTL values (see table); invalid values are rejected by the API.

## Namecheap (`namecheap.so`)

- Configuration name: `namecheap`
- Supported records: A

Updates DNS records via the [Namecheap Dynamic DNS API](https://www.namecheap.com/support/knowledgebase/article.aspx/29/11/how-to-configure-your-dns-dynamic-dns-update-url/) using a GET-based update endpoint.

- Supports **A (IPv4) records only**. AAAA (IPv6) records are not supported by the upstream API.

Requires **libxml2** at build time. If libxml2 is not found, the driver is skipped with a CMake warning.

| Parameter  | Required | Description                                                                                         |
|------------|----------|-----------------------------------------------------------------------------------------------------|
| `password` | Yes      | Dynamic DNS password from Namecheap's Advanced DNS tab → Dynamic DNS section (not your account password) |

## Porkbun (`porkbun.so`)

- Configuration name: `porkbun`
- Supported records: A, AAAA

Updates DNS records via the [Porkbun API v3](https://porkbun.com/api/json/v3/documentation) using API key + Secret key authentication.

- Supports **A (IPv4)** and **AAAA (IPv6)** records.

| Parameter        | Required | Description                                              |
|------------------|----------|----------------------------------------------------------|
| `api_key`        | Yes      | Porkbun API key                                          |
| `secret_api_key` | Yes      | Porkbun Secret API key                                   |
| `ttl`            | No       | TTL in seconds (default: account minimum)                |

**Note:** The driver uses the `editByNameType` endpoint. If multiple records of the same type exist for the same subdomain, the update behaviour is undefined.

## Route 53 (`route53.so`)

- Configuration name: `route53`
- Supported records: A, AAAA

Updates DNS records via the [AWS Route 53 ChangeResourceRecordSets API](https://docs.aws.amazon.com/Route53/latest/APIReference/API_ChangeResourceRecordSets.html) using SigV4 request signing.

- Supports **A (IPv4)** and **AAAA (IPv6)** records.

Requires **libxml2** at build time. If libxml2 is not found, the driver is skipped with a CMake warning.

| Parameter | Required | Description                                                    |
|-----------|----------|----------------------------------------------------------------|
| `access_key_id` | Yes | AWS access key ID                                              |
| `secret_access_key` | Yes | AWS secret access key                                   |
| `hosted_zone_id` | Yes | Route 53 hosted zone ID (e.g. `Z3M79L5CQABCDE`)               |
| `region`   | Yes      | AWS region (e.g. `us-east-1`)                                  |
| `record_name` | Yes   | DNS record name (subdomain to update)                          |
| `ttl`      | No       | TTL in seconds (default: 300)                                  |

**Note:** The driver uses the UPSERT action — it creates the record if it does not exist. The FQDN trailing dot is handled automatically.

## Simple (`simple.so`)

- Configuration name: `simple`
- Supported records: API-dependent

A generic HTTP GET driver for custom APIs. The driver treats the `url` as a template and substitutes `{key}` placeholders with values from the configuration and runtime context.

| Parameter | Required | Description                                                                                                              |
|-----------|----------|--------------------------------------------------------------------------------------------------------------------------|
| `url`     | Yes      | HTTP(S) URL template with `{key}` placeholders. All other `driver_param` keys are available for substitution as `{key}`. |

**Available substitution variables:**

| Variable      | Source         | Description                                |
|---------------|----------------|--------------------------------------------|
| `{ip_addr}`   | Runtime        | The detected IP address                    |
| `{rd_type}`   | Runtime        | DNS record type (A, AAAA, TXT)             |
| `{domain}`    | Runtime        | Domain name                                |
| `{subdomain}` | Runtime        | Subdomain name                             |
| `{fqdn}`      | Runtime        | Full domain name                           |
| `{any_key}`   | `driver_param` | Any key from `driver_param` (except `url`) |

Example:
```json
{
  "driver_param": {
    "url": "https://api.example.com/update?ip={ip_addr}&type={rd_type}&domain={domain}",
    "key": "my-secret-key"
  }
}
```

A successful response is any non-empty body. The request method is always GET
and this driver has no header configuration. Do not put long-lived credentials
in the URL when an API offers a safer authentication mechanism.

## Vultr (`vultr.so`)

- Configuration name: `vultr`
- Supported records: A, AAAA

Updates DNS records via the [Vultr API v2](https://www.vultr.com/api/#tag/dns) using Bearer token authentication.

- Supports **A (IPv4)** and **AAAA (IPv6)** records (updates existing records; cannot change record type).

| Parameter   | Required | Description                                              |
|-------------|----------|----------------------------------------------------------|
| `api_key`   | Yes      | Vultr API key (Bearer token)                             |
| `record_id` | Yes      | DNS Record ID to update                                  |
| `ttl`       | No       | TTL in seconds (default: server default)                 |

**Note:** The driver uses HTTP PATCH (less commonly supported by proxies). A successful update returns HTTP 204 No Content.
