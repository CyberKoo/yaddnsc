//
// Created by Kotarou on 2022/4/5.
//

#include <resolv.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <arpa/nameser.h>
#include <netdb.h>

#include "dns.h"
#include "util.h"

const int MAXIMUM_UDP_SIZE = 512;

std::string errno_string(int error) {
    switch (error) {
        case HOST_NOT_FOUND:
            return "NXDOMAIN";
        case NO_DATA:
            return "NO_DATA";
        case TRY_AGAIN:
            return "TRY_AGAIN";
        default:
            return "Unknown Error";
    }
}

std::vector<std::string> DNS::resolve(std::string_view host, dns_record_t type) {
    return resolve(host, type, std::nullopt);
}

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
            throw std::runtime_error(errno_string(h_errno));
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
            throw std::runtime_error(strerror(errno));
        }

        auto rr_type = ns_rr_type(rr);
        std::array<char, INET6_ADDRSTRLEN> address_buffer{};
        if (rr_type == ns_t_a) {
            inet_ntop(AF_INET, ns_rr_rdata(rr), address_buffer.data(), address_buffer.size());
            resolve_result.emplace_back(address_buffer.data());
        } else if (rr_type == ns_t_aaaa) {
            inet_ntop(AF_INET6, ns_rr_rdata(rr), address_buffer.data(), address_buffer.size());
            resolve_result.emplace_back(address_buffer.data());
        }
    }

    return resolve_result;
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
