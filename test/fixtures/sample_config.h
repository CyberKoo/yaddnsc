//
// Shared sample configuration JSON strings for config parser / validator tests.
//
// Each sample exercises a specific parsing or validation scenario.
// =============================================================================

#ifndef YADDNSC_TEST_FIXTURES_SAMPLE_CONFIG_H
#define YADDNSC_TEST_FIXTURES_SAMPLE_CONFIG_H

#include <string_view>

namespace Fixtures {

// ── Minimal valid config ─────────────────────────────────────────────────────

inline constexpr std::string_view MINIMAL_CONFIG = R"({
    "driver": {
        "auto_discover": true
    },
    "resolver": {
        "use_custom_server": false
    },
    "domains": []
})";

// ── Full config with one domain, two subdomains ──────────────────────────────

inline constexpr std::string_view FULL_CONFIG = R"({
    "driver": {
        "driver_dir": "/usr/lib/yaddnsc/drivers",
        "auto_discover": true,
        "load": ["cloudflare", "digital_ocean"]
    },
    "resolver": {
        "use_custom_server": true,
        "servers": [
            {"address": "1.1.1.1", "port": 53},
            {"address": "8.8.8.8", "port": 53}
        ],
        "strategy": "fallback"
    },
    "domains": [
        {
            "name": "example.com",
            "update_interval": 300,
            "force_update": 3600,
            "driver": "cloudflare",
            "subdomains": [
                {"name": "@", "type": "a", "ip_source": "http", "ip_source_param": "https://api.ipify.org"},
                {"name": "www", "type": "aaaa", "ip_source": "interface", "interface": "eth0", "ip_type": "ipv6"}
            ]
        }
    ]
})";

// ── Config with mDNS IP source ───────────────────────────────────────────────

inline constexpr std::string_view MDNS_CONFIG = R"({
    "driver": { "auto_discover": true },
    "resolver": { "use_custom_server": false },
    "domains": [
        {
            "name": "example.com",
            "update_interval": 60,
            "driver": "simple",
            "subdomains": [
                {"name": "printer", "type": "a", "ip_source": "mdns", "ip_source_param": "printer.local"}
            ]
        }
    ]
})";

// ── Config with backward-compatible "ipaddress" and "url" keys ───────────────

inline constexpr std::string_view BACKWARD_COMPAT_CONFIG = R"({
    "driver": { "auto_discover": true },
    "resolver": {
        "use_custom_server": true,
        "ipaddress": "9.9.9.9",
        "port": 53,
        "strategy": "concurrent"
    },
    "domains": [
        {
            "name": "example.org",
            "update_interval": 120,
            "driver": "digital_ocean",
            "subdomains": [
                {"name": "blog", "type": "aaaa", "ip_source": "url", "ip_source_param": "https://api6.ipify.org"}
            ]
        }
    ]
})";

// ── Config with all SubdomainConfig fields ───────────────────────────────────

inline constexpr std::string_view ALL_SUBDOMAIN_FIELDS = R"({
    "driver": { "auto_discover": true },
    "resolver": { "use_custom_server": false },
    "domains": [
        {
            "name": "test.net",
            "update_interval": 300,
            "force_update": 1800,
            "driver": "cloudflare",
            "subdomains": [
                {
                    "name": "api",
                    "type": "txt",
                    "interface": "bond0",
                    "ip_type": "unspecified",
                    "ip_source": "http",
                    "ip_source_param": "https://checkip.amazonaws.com",
                    "allow_ula": true,
                    "allow_local_link": false,
                    "update_interval": 60,
                    "driver_param": {"zone_id": "abc123"}
                }
            ]
        }
    ]
})";

// ── Config with force_update = 0 (force update disabled) ─────────────────────

inline constexpr std::string_view NO_FORCE_UPDATE_CONFIG = R"({
    "driver": { "auto_discover": true },
    "resolver": { "use_custom_server": false },
    "domains": [
        {
            "name": "example.com",
            "update_interval": 300,
            "force_update": 0,
            "driver": "cloudflare",
            "subdomains": [
                {"name": "@", "type": "a", "ip_source": "http", "ip_source_param": "https://api.ipify.org"}
            ]
        }
    ]
})";

// ── Config with empty domain list (no error but no work to do) ───────────────

inline constexpr std::string_view EMPTY_DOMAINS_CONFIG = R"({
    "driver": { "driver_dir": "./drivers", "auto_discover": false, "load": [] },
    "resolver": { "use_custom_server": false },
    "domains": []
})";

// ── Invalid configs for parser error-path testing ────────────────────────────

inline constexpr std::string_view INVALID_JSON = R"({
    "driver": {
        "auto_discover": true
    },
    INVALID
})";

inline constexpr std::string_view MISSING_REQUIRED_FIELD = R"({
    "resolver": { "use_custom_server": false },
    "domains": []
})";

inline constexpr std::string_view WRONG_TYPE_VALUE = R"({
    "driver": { "auto_discover": "not_a_boolean" },
    "resolver": { "use_custom_server": false },
    "domains": []
})";

// ── DNS response samples (wire-format) for parser tests ──────────────────────

namespace DnsWire {

// Minimal DNS response header: 12 bytes
//   ID=0x1234, QR=1, OpCode=0, AA=0, TC=0, RD=1, RA=1, Z=0, RCODE=0
//   QDCOUNT=1, ANCOUNT=1, NSCOUNT=0, ARCOUNT=0
inline constexpr unsigned char SIMPLE_A_RESPONSE[] = {
    0x12, 0x34,                         // ID
    0x81, 0x80,                         // flags: QR, RD, RA
    0x00, 0x01,                         // QDCOUNT
    0x00, 0x01,                         // ANCOUNT
    0x00, 0x00,                         // NSCOUNT
    0x00, 0x00,                         // ARCOUNT
    // Question: example.com A
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,                               // end of name
    0x00, 0x01,                         // QTYPE A
    0x00, 0x01,                         // QCLASS IN
    // Answer: example.com A 192.0.2.1 TTL=300
    0xC0, 0x0C,                         // name pointer to offset 12
    0x00, 0x01,                         // TYPE A
    0x00, 0x01,                         // CLASS IN
    0x00, 0x00, 0x01, 0x2C,            // TTL 300
    0x00, 0x04,                         // RDLENGTH 4
    0xC0, 0x00, 0x02, 0x01             // RDATA 192.0.2.1
};

// NXDOMAIN response header
inline constexpr unsigned char NXDOMAIN_RESPONSE[] = {
    0x12, 0x35,                         // ID
    0x81, 0x83,                         // flags: QR, RD, RA, RCODE=3 (NXDOMAIN)
    0x00, 0x01,                         // QDCOUNT
    0x00, 0x00,                         // ANCOUNT
    0x00, 0x01,                         // NSCOUNT
    0x00, 0x00,                         // ARCOUNT
    // Question: nonexistent.example.com AAAA
    0x0A, 'n', 'o', 'n', 'e', 'x', 'i', 's', 't', 'e', 'n', 't',
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,                               // end of name
    0x00, 0x1C,                         // QTYPE AAAA
    0x00, 0x01                          // QCLASS IN
};

// SERVFAIL response header
inline constexpr unsigned char SERVFAIL_RESPONSE[] = {
    0x12, 0x36,                         // ID
    0x81, 0x82,                         // flags: QR, RD, RA, RCODE=2 (SERVFAIL)
    0x00, 0x01,                         // QDCOUNT
    0x00, 0x00,                         // ANCOUNT
    0x00, 0x00,                         // NSCOUNT
    0x00, 0x00,                         // ARCOUNT
    // Question: example.com A
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01,
    0x00, 0x01
};

} // namespace DnsWire
} // namespace Fixtures

#endif // YADDNSC_TEST_FIXTURES_SAMPLE_CONFIG_H
