//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_PARSER_H
#define YADDNSC_DNS_PARSER_H

#include <cstdint>
#include <string>
#include <vector>

#include "type.h"

// Parse raw DNS response bytes (wire format) into a vector of record strings.
// Supports A, AAAA, TXT, NS, SOA, CNAME, MX record types.
std::vector<std::string> parse_dns_response(const uint8_t *data, size_t size);

// Convert dns_record_type to <arpa/nameser.h> ns_t_* constant.
int dns_type_to_ns_type(dns_record_type type) noexcept;

#endif // YADDNSC_DNS_PARSER_H
