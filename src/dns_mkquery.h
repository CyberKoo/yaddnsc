//
// Created by Kotarou on 2026/6/29.
//

#ifndef YADDNSC_DNS_MKQUERY_H
#define YADDNSC_DNS_MKQUERY_H

#include <cstdint>
#include <string>
#include <vector>

[[nodiscard]] std::vector<uint8_t> dns_mkquery(const std::string &host, int ns_type);
[[nodiscard]] std::vector<uint8_t> dns_mkquery_manual(const std::string &host, int ns_type);
[[nodiscard]] std::vector<uint8_t> dns_mkquery_system(const std::string &host, int ns_type);

#endif // YADDNSC_DNS_MKQUERY_H
