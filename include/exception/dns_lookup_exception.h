//
// Created by Kotarou on 2022/4/9.
//

#ifndef YADDNSC_DNS_LOOKUP_EXCEPTION_H
#define YADDNSC_DNS_LOOKUP_EXCEPTION_H

#include "base_exception.h"
#include "type.h"

class DnsLookupException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    DnsLookupException(const std::string &msg, dns_error err) : YaddnscException(msg), error(err) {}

    DnsLookupException(const char *msg, dns_error err) : YaddnscException(msg), error(err) {}

    DnsLookupException(YaddnscException &&l_error, dns_error err) : YaddnscException(l_error),
                                                                                  error(err) {}

    DnsLookupException(const YaddnscException &l_error, dns_error err) : YaddnscException(l_error),
                                                                                       error(err) {}

    [[nodiscard]] std::string_view get_name() const override {
        return "DnsLookupException";
    }

    [[nodiscard]] dns_error get_error() const {
        return error;
    }

private:
    dns_error error;
};


#endif //YADDNSC_DNS_LOOKUP_EXCEPTION_H
