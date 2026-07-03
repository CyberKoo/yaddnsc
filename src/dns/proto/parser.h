//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_PARSER_H
#define YADDNSC_DNS_PARSER_H

#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

#include <arpa/nameser.h>

namespace DNS {

class DnsRecordParser {
public:
    using data_type = uint8_t;

    explicit DnsRecordParser(const data_type *data, size_t size);

    [[nodiscard]] size_t record_count() const noexcept;

    [[nodiscard]] std::string parse_record(size_t index) const;

    // Convenience: parse all answer records and return as a vector.
    // This is the preferred entry point for most callers.
    [[nodiscard]] static std::vector<std::string>
    parse_all(const data_type *data, size_t size, const std::string &host = {});

private:
    static std::string parse_a_record(const data_type *);

    static std::string parse_aaaa_record(const data_type *);

    static std::string parse_txt_record(const data_type *, int);

    static std::string parse_domain_name_record(const data_type *, const data_type *, const data_type *);

    static std::string parse_mx_record(const data_type *, const data_type *, const data_type *);

    mutable ns_msg message_{};
};

} // namespace DNS

#endif // YADDNSC_DNS_PARSER_H
