//
// Created by Kotarou on 2022/4/9.
//

#ifndef YADDNSC_DNS_LOOKUP_EXCEPTION_H
#define YADDNSC_DNS_LOOKUP_EXCEPTION_H

#include "base_exception.h"
#include "../../src/type.h"

class DnsLookupException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    DnsLookupException(const std::string &msg, dns_lookup_error_t error) : YaddnscException(msg), error(error) {}

    DnsLookupException(const char *msg, dns_lookup_error_t error) : YaddnscException(msg), error(error) {}

    DnsLookupException(YaddnscException &&l_error, dns_lookup_error_t error) : YaddnscException(l_error),
                                                                               error(error) {}

    DnsLookupException(const YaddnscException &l_error, dns_lookup_error_t error) : YaddnscException(l_error),
                                                                                    error(error) {}

    [[nodiscard]] std::string_view get_name() const override {
        return "DnsLookupException";
    }

    [[nodiscard]] dns_lookup_error_t get_error() const {
        return error;
    }

private:
    dns_lookup_error_t error;
};


#endif //YADDNSC_DNS_LOOKUP_EXCEPTION_H
