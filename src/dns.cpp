//
// Created by Kotarou on 2022/4/5.
//

#include "dns.h"

#include <netdb.h>
#include <resolv.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/nameser.h>
#include <spdlog/spdlog.h>

#include "exception/dns_lookup_exception.h"

std::vector<std::string> DNS::resolve(std::string_view host, dns_record_t type, std::optional<dns_server_t> server) {
    SPDLOG_DEBUG("Resolve domain \"{}\"", host);
    struct __res_state local_res{};
    res_ninit(&local_res);

    if (server.has_value() && !server->ip_address.empty()) {
        SPDLOG_DEBUG("Use customized resolver \"{}:{}\"", server->ip_address, server->port);

        local_res.nscount = 1;
        local_res.nsaddr_list[0].sin_family = AF_INET;
        local_res.nsaddr_list[0].sin_addr.s_addr = inet_addr(server->ip_address.c_str());
        local_res.nsaddr_list[0].sin_port = htons(server->port);
    }

    int size = 0;
    int buffer_size = MAXIMUM_UDP_SIZE;
    std::unique_ptr<unsigned char[]> buffer;

    do {
        // adjust buffer size
        buffer_size = size + buffer_size;
        // allocate buffer
        buffer = std::make_unique<unsigned char[]>(buffer_size);
        // perform dns lookup
        size = res_nquery(&local_res, host.data(), ns_c_in, get_dns_type(type), buffer.get(), buffer_size);
        if (size < 0) {
            res_nclose(&local_res);
            throw DnsLookupException(host.data(), get_dns_lookup_err(h_errno));
        }
        SPDLOG_DEBUG("Response payload size: {}, buffer size: {}", size, buffer_size);
    } while (size >= buffer_size);
    res_nclose(&local_res);

    ns_msg handle;
    ns_initparse(buffer.get(), buffer_size, &handle);
    auto msg_count = ns_msg_count(handle, ns_s_an);

    std::vector<std::string> resolve_result;
    for (int i = 0; i < msg_count; i++) {
        ns_rr rr;
        if (ns_parserr(&handle, ns_s_an, i, &rr)) {
            throw DnsLookupException(fmt::format("Error occurred when parse dns resource, {}", strerror(errno)),
                                     dns_lookup_error_t::PARSE);
        }

        auto rr_type = ns_rr_type(rr);
        char address_buffer[INET6_ADDRSTRLEN] = {};
        if (rr_type == ns_t_a) {
            inet_ntop(AF_INET, ns_rr_rdata(rr), address_buffer, INET6_ADDRSTRLEN);
            resolve_result.emplace_back(address_buffer);
        } else if (rr_type == ns_t_aaaa) {
            inet_ntop(AF_INET6, ns_rr_rdata(rr), address_buffer, INET6_ADDRSTRLEN);
            resolve_result.emplace_back(address_buffer);
        }
    }

    return resolve_result;
}

dns_lookup_error_t DNS::get_dns_lookup_err(int error) {
    switch (error) {
        case HOST_NOT_FOUND:
            return dns_lookup_error_t::NX_DOMAIN;
        case NO_DATA:
            return dns_lookup_error_t::NODATA;
        case TRY_AGAIN:
            return dns_lookup_error_t::RETRY;
        default:
            return dns_lookup_error_t::UNKNOWN;
    }
}

int DNS::get_dns_type(dns_record_t type) {
    switch (type) {
        case dns_record_t::A:
            return ns_t_a;
        case dns_record_t::AAAA:
            return ns_t_aaaa;
        case dns_record_t::TXT:
            return ns_t_txt;
        default:
            return ns_t_soa;
    }
}

std::string_view DNS::error_to_str(dns_lookup_error_t error) {
    switch (error) {
        case dns_lookup_error_t::NX_DOMAIN:
            return "No such domain";
        case dns_lookup_error_t::RETRY:
            return "retry";
        case dns_lookup_error_t::NODATA:
            return "no data";
        case dns_lookup_error_t::PARSE:
            return "dns record parse error";
        default:
            return "unknown error";
    }
}
