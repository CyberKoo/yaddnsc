# DNS Provider Drivers

yaddnsc ships twelve provider drivers as loadable modules. This document is
the parameter reference for each of them. For installation and general
configuration see [README.md](README.md); for developing additional drivers
see [docs/custom-drivers.md](docs/custom-drivers.md).

## General Rules

- A domain selects its driver through the `driver` field, using the
  configuration name listed below.
- Parameters are supplied in `driver_param`, which is valid only inside a
  subdomain entry. Each record carries a complete `driver_param` of its own;
  there is no inheritance from the domain level.
- Every driver validates its `driver_param` during `yaddnsc config test`.
  A missing required key or an unrecognized key is a validation error. The
  `simple` driver is the exception: it accepts arbitrary additional keys.
- All drivers update an existing record; create the record with the provider
  before referencing it. The sole exception is `route53`, which creates the
  record when it does not exist.
- No bundled driver updates TXT records. The `txt` type is only meaningful to
  the `yaddnsc dns resolve` diagnostic command.
- Keep credentials out of version control, restrict the configuration file
  (for example `chmod 600 config.json`), and grant each credential only the
  permissions the update requires.

| Configuration name | Module file |
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

## Capability Summary

| Provider | Driver | A | AAAA | Record handling |
|---|---|---:|---:|---|
| Alibaba Cloud | `alibaba_cloud` | Yes | Yes | Updates an existing record |
| Cloudflare | `cloudflare` | Yes | Yes | Updates an existing record |
| DigitalOcean | `digital_ocean` | Yes | Yes | Updates an existing record |
| DNSPod | `dnspod` | Yes | Yes | Updates an existing record |
| DuckDNS | `duckdns` | Yes | Yes | DuckDNS-registered names only |
| GoDaddy | `godaddy` | Yes | Yes | Replaces the record set for name and type |
| Linode | `linode` | Yes | Yes | Updates an existing record |
| Namecheap | `namecheap` | Yes | — | Updates an existing record |
| Porkbun | `porkbun` | Yes | Yes | Selects the record by name and type |
| Route 53 | `route53` | Yes | Yes | Creates the record when absent |
| Simple | `simple` | Yes | Yes | Generic HTTP endpoint |
| Vultr | `vultr` | Yes | Yes | Updates an existing record |

Provider-side constraints (account plans, rate limits, API quotas) apply in
addition to the behaviour described here.

Contents:

- [Alibaba Cloud](#alibaba-cloud-alibaba_cloud)
- [Cloudflare](#cloudflare-cloudflare)
- [DigitalOcean](#digitalocean-digital_ocean)
- [DNSPod](#dnspod-dnspod)
- [DuckDNS](#duckdns-duckdns)
- [GoDaddy](#godaddy-godaddy)
- [Linode](#linode-linode)
- [Namecheap](#namecheap-namecheap)
- [Porkbun](#porkbun-porkbun)
- [Route 53](#route-53-route53)
- [Simple](#simple-simple)
- [Vultr](#vultr-vultr)

---

## Alibaba Cloud (`alibaba_cloud`)

Records: A, AAAA (existing record). Uses the
[Alibaba Cloud DNS API](https://www.alibabacloud.com/help/en/dns/api-alidns-2015-01-09-updatedomainrecord)
with AccessKey-signed requests.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `access_key_id` | Yes | — | Alibaba Cloud AccessKey ID |
| `access_key_secret` | Yes | — | Alibaba Cloud AccessKey Secret |
| `record_id` | Yes | — | ID of the record to update |
| `ttl` | No | 600 | TTL in seconds |

Only the international endpoint is supported; the China endpoint is not
available.

## Cloudflare (`cloudflare`)

Records: A, AAAA (existing record). Uses the
[Cloudflare API v4](https://developers.cloudflare.com/api/) with an API token.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `zone_id` | Yes | — | Zone ID of the domain |
| `record_id` | Yes | — | ID of the record to update |
| `token` | Yes | — | API token |
| `ttl` | No | 30 | TTL in seconds |
| `proxied` | No | `false` | Route the record through the Cloudflare proxy |

The token must hold the DNS edit permission for the zone. Zone and record IDs
are shown in the Cloudflare dashboard and through the API.

## DigitalOcean (`digital_ocean`)

Records: A, AAAA (existing record). Uses the
[DigitalOcean API v2](https://developers.digitalocean.com/documentation/v2/)
with a personal access token.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `record_id` | Yes | — | ID of the record to update |
| `token` | Yes | — | Personal access token |

Only the record content is modified; the driver exposes no TTL control.

## DNSPod (`dnspod`)

Records: A, AAAA (existing record). Uses the
[DNSPod API](https://www.dnspod.com/docs/) (`Record.Ddns`) with a login token.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `domain_id` | Yes | — | Domain ID |
| `record_id` | Yes | — | Record ID |
| `login_token` | Yes | — | API token in `ID,Token` format |
| `global` | Yes | — | `false` selects the China endpoint, `true` the international endpoint |
| `record_line_id` | Yes | — | Record line ID; use `"0"` for the default line |
| `record_line` | No | `"默认"` (China) / `"default"` (international) | Record line name |

`global` and `record_line_id` behave like fixed settings, yet the driver
treats them as required keys: omitting either one fails validation. The
`login_token` must be comma-separated in `ID,Token` form. The API enforces
rate limiting; requests rejected with error code `-2` indicate excessive
frequency.

## DuckDNS (`duckdns`)

Records: A, AAAA. Uses the [DuckDNS](https://www.duckdns.org/) update
endpoint with a per-account token.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `token` | Yes | — | Account token from the DuckDNS site |
| `verbose` | No | `false` | Request a verbose response for additional logging |

The driver applies only to names registered with DuckDNS: the subdomain label
of the configured record must be the DuckDNS host name. No TTL control
exists.

## GoDaddy (`godaddy`)

Records: A, AAAA. Uses the
[GoDaddy Domains API v1](https://developer.godaddy.com/doc/endpoint/domains)
with a key/secret pair.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `key` | Yes | — | API key |
| `secret` | Yes | — | API secret |
| `ttl` | No | 600 | TTL in seconds |

An update replaces the record set for the configured name and type.

## Linode (`linode`)

Records: A, AAAA (existing record). Uses the
[Linode API v4](https://techdocs.akamai.com/linode-api/reference/put-domain-record)
with a personal access token.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `token` | Yes | — | Personal access token |
| `domain_id` | Yes | — | Domain ID |
| `record_id` | Yes | — | ID of the record to update |
| `ttl_sec` | No | unchanged | TTL in seconds; omitted from the request when unset |

The Linode API accepts only a fixed set of TTL values and rejects others; an
invalid `ttl_sec` surfaces as an API error at update time.

## Namecheap (`namecheap`)

Records: A only. Uses the
[Namecheap Dynamic DNS API](https://www.namecheap.com/support/knowledgebase/article.aspx/29/11/how-to-configure-your-dns-dynamic-dns-update-url/).

| Parameter | Required | Default | Description |
|---|---|---|---|
| `password` | Yes | — | Dynamic DNS password from the Namecheap panel (Advanced DNS), not the account password |

The upstream API does not support AAAA records; AAAA updates are rejected
before any request is made. This driver is built only when libxml2 is
available at build time.

## Porkbun (`porkbun`)

Records: A, AAAA. Uses the
[Porkbun API v3](https://porkbun.com/api/json/v3/documentation) with an API
key pair.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `api_key` | Yes | — | API key |
| `secret_api_key` | Yes | — | Secret API key |
| `ttl` | No | account minimum | TTL in seconds; omitted from the request when unset |

The record is located by name and type. If several records share the same
name and type, which of them is updated is undefined; keep at most one record
per name and type.

## Route 53 (`route53`)

Records: A, AAAA. Uses the
[AWS Route 53 API](https://docs.aws.amazon.com/Route53/latest/APIReference/API_ChangeResourceRecordSets.html)
with SigV4-signed requests.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `access_key_id` | Yes | — | AWS access key ID |
| `secret_access_key` | Yes | — | AWS secret access key |
| `hosted_zone_id` | Yes | — | Hosted zone ID |
| `region` | Yes | — | AWS region used for request signing |
| `record_name` | Yes | — | Record name; see note below |
| `ttl` | No | 300 | TTL in seconds |

The driver issues an UPSERT: the record is created when it does not exist.
The record acted upon is always the fully qualified name formed by the
configured domain and subdomain; `record_name` must be present for validation
but its value is not used — setting it to that same name avoids confusion.
This driver is built only when libxml2 is available at build time.

## Simple (`simple`)

Records: A, AAAA. A generic driver for custom HTTP APIs: it issues a GET
request to a URL built from a template.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `url` | Yes | — | HTTP(S) URL template containing `{key}` placeholders |

Any additional string-valued key in `driver_param` becomes a substitution
variable `{key}`. The following built-in variables are always available:

| Variable | Value |
|---|---|
| `{ip_addr}` | The detected IP address |
| `{rd_type}` | Record type (`A`, `AAAA`) |
| `{domain}` | Domain name |
| `{subdomain}` | Subdomain label |
| `{fqdn}` | Fully qualified record name |

Example:

```json
{
  "driver_param": {
    "url": "https://api.example.com/update?ip={ip_addr}&type={rd_type}&name={fqdn}",
    "key": "my-secret-key"
  }
}
```

An update succeeds when the endpoint answers with an HTTP status below 300
and a non-empty body. The request method is always GET and no headers can be
configured; where an API offers a stronger authentication mechanism, prefer
it over credentials embedded in the URL.

## Vultr (`vultr`)

Records: A, AAAA (existing record). Uses the
[Vultr API v2](https://www.vultr.com/api/#tag/dns) with an API key.

| Parameter | Required | Default | Description |
|---|---|---|---|
| `api_key` | Yes | — | API key |
| `record_id` | Yes | — | ID of the record to update |
| `ttl` | No | server default | TTL in seconds; omitted from the request when unset |

Updates are issued as HTTP PATCH requests, which some proxies do not forward;
a successful update returns HTTP 204.
