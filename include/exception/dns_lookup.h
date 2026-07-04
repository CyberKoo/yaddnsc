//
// Created by Kotarou on 2022/4/9.
//

#ifndef YADDNSC_EXCEPTION_DNS_LOOKUP_H
#define YADDNSC_EXCEPTION_DNS_LOOKUP_H

#include "base.h"
#include "dns_error.h"

class DnsLookupException : public YaddnscException {
public:
    using YaddnscException::YaddnscException;

    DnsLookupException(const std::string &msg, DNS::Error err) : YaddnscException(msg), error_(err) {
    }

    DnsLookupException(const char *msg, DNS::Error err) : YaddnscException(msg), error_(err) {
    }

    DnsLookupException(YaddnscException &&exc, DNS::Error err) : YaddnscException(exc), error_(err) {
    }

    DnsLookupException(const YaddnscException &exc, DNS::Error err) : YaddnscException(exc), error_(err) {
    }

    [[nodiscard]] std::string_view get_name() const noexcept override {
        return "DnsLookupException";
    }

    [[nodiscard]] DNS::Error get_error() const noexcept {
        return error_;
    }

private:
    DNS::Error error_;
};


#endif //YADDNSC_EXCEPTION_DNS_LOOKUP_H
