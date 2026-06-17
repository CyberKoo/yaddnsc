//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_RECORD_PARSER_H
#define YADDNSC_DNS_RECORD_PARSER_H

#include <string>
#include <vector>
#include <cstddef>
#include <string_view>

#include <arpa/nameser.h>
#include <arpa/inet.h>

class DnsRecordParser {
public:
    explicit DnsRecordParser(const unsigned char *data, size_t size);

    [[nodiscard]] size_t record_count() const noexcept;
    [[nodiscard]] std::string parse_record(size_t index) const;

private:
    static std::string parse_a_record(const unsigned char *rdata);
    static std::string parse_aaaa_record(const unsigned char *rdata);
    static std::string parse_txt_record(const unsigned char *rdata, int rdlen);
    static std::string parse_domain_name_record(const unsigned char *msg_base,
                                                const unsigned char *msg_end,
                                                const unsigned char *rdata);
    static std::string parse_mx_record(const unsigned char *msg_base,
                                       const unsigned char *msg_end,
                                       const unsigned char *rdata);

    ns_msg message_{};
};

#endif //YADDNSC_DNS_RECORD_PARSER_H
