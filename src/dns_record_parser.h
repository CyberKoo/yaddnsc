//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_RECORD_PARSER_H
#define YADDNSC_DNS_RECORD_PARSER_H

#include <string>
#include <string_view>

#include <arpa/nameser.h>

class DnsRecordParser {
public:
    using data_type = const unsigned char;

    explicit DnsRecordParser(data_type *data, size_t size);

    [[nodiscard]] size_t record_count() const noexcept;

    [[nodiscard]] std::string parse_record(size_t index) const;

private:

    static std::string parse_a_record(data_type *rdata);

    static std::string parse_aaaa_record(data_type *rdata);

    static std::string parse_txt_record(data_type *rdata, int rdlen);

    static std::string parse_domain_name_record(data_type *msg_base, data_type *msg_end, data_type *rdata);

    static std::string parse_mx_record(data_type *msg_base, data_type *msg_end, data_type *rdata);

    mutable ns_msg message_{};
};

#endif //YADDNSC_DNS_RECORD_PARSER_H
