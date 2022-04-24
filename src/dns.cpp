//
// Created by Kotarou on 2022/4/5.
//

#include "dns.h"

#include <utility>
#include <spdlog/spdlog.h>

#include <arpa/nameser.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <resolv.h>
#include <netdb.h>

#include "ip_util.h"
#include "exception/dns_lookup_exception.h"

constexpr static int MAXIMUM_UDP_SIZE = 512;

using resolve_result = std::vector<std::string>;

using query_result = std::tuple<std::unique_ptr<unsigned char[]>, int>;

dns_lookup_error_t get_dns_lookup_err(int);

int get_dns_type(dns_record_t);

query_result query(std::string_view, dns_record_t, const std::optional<dns_server_t> &);

resolve_result DNS::resolve(std::string_view host, dns_record_t type, const std::optional<dns_server_t> &server) {
    auto [buffer, buffer_size] = query(host, type, server);

    ns_msg handle{};
    ns_initparse(buffer.get(), buffer_size, &handle);
    auto msg_count = ns_msg_count(handle, ns_s_an);

    resolve_result resolve_result;
    for (int i = 0; i < msg_count; i++) {
        ns_rr rr{};
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

query_result
query(std::string_view host, dns_record_t type, [[maybe_unused]]const std::optional<dns_server_t> &server) {
    int buffer_size = MAXIMUM_UDP_SIZE;

#ifdef HAVE_RES_NQUERY
    SPDLOG_DEBUG(R"(Resolve domain "{}")", host);
    struct __res_state local_state{};
    res_ninit(&local_state);

#ifdef HAVE_RES_NDESTROY
    std::unique_ptr<struct __res_state, decltype(&res_ndestroy)> state_ptr(&local_state, res_ndestroy);
#else
    std::unique_ptr<struct __res_state, decltype(&res_nclose)> state_ptr(&local_state, res_nclose);
#endif

    if (server.has_value() && !server->ip_address.empty()) {
        if (IPUtil::is_ipv4_address(server->ip_address)) {
            local_state.nscount = 1;
            local_state.nsaddr_list[0].sin_family = AF_INET;
            local_state.nsaddr_list[0].sin_addr.s_addr = inet_addr(server->ip_address.c_str());
            local_state.nsaddr_list[0].sin_port = htons(server->port);
        } else {
            // zero out all settings from resolv.conf
            for (auto i = 0; i < local_state.nscount; ++i) {
                std::memset(&local_state.nsaddr_list[i], 0, sizeof(local_state.nsaddr_list[i]));
            }

            local_state.nscount = 1;
            local_state._u._ext.nscount = 1;
#ifdef HAVE_RES_STATE_EXT_NSADDRS // glibc
            local_state._u._ext.nscount6 = 1;
            local_state._u._ext.nsmap[0] = MAXNS + 1;
            struct sockaddr_in6 *sa6 = local_state._u._ext.nsaddrs[0];
            if (sa6 == nullptr) {
                // Memory allocated here will be free'd in res_nclose() as we have done res_ninit() above.
                sa6 = reinterpret_cast<struct sockaddr_in6 *>(calloc(1, sizeof(struct sockaddr_in6)));
                local_state._u._ext.nsaddrs[0] = sa6;
            }

            sa6->sin6_port = htons(server->port);
            sa6->sin6_family = AF_INET6;
            inet_pton(AF_INET6, server->ip_address.c_str(), &sa6->sin6_addr);
#elif HAVE_RES_SETSERVERS // bsd-libc (*BSD & MacOS)
            res_sockaddr_union sau{};

            sau.sin6.sin6_port = htons(server->port);
            sau.sin6.sin6_family = AF_INET6;
            inet_pton(AF_INET6, server->ip_address.c_str(), &sau.sin6.sin6_addr);
            res_setservers(&local_state, &sau, 1);
#endif
        }
    }
#endif

    int received_size = 0;
    std::unique_ptr<unsigned char[]> buffer;
    do {
        // adjust buffer size
        buffer_size = received_size + buffer_size;
        // allocate buffer
        buffer = std::make_unique<unsigned char[]>(buffer_size);
        // perform dns lookup
#ifdef HAVE_RES_NQUERY
        received_size = res_nquery(&local_state, host.data(), ns_c_in, get_dns_type(type), buffer.get(), buffer_size);
#else
        received_size = res_query(host.data(), ns_c_in, get_dns_type(type), buffer.get(), buffer_size);
#endif
        if (received_size < 0) {
            throw DnsLookupException(host.data(), get_dns_lookup_err(h_errno));
        }
        SPDLOG_DEBUG("Response payload size: {}, buffer size: {}", received_size, buffer_size);
    } while (received_size >= buffer_size);

    return {std::move(buffer), buffer_size};
}

std::string_view DNS::error_to_str(dns_lookup_error_t error) {
    switch (error) {
        case dns_lookup_error_t::NX_DOMAIN:
            return "no such domain (NXDOMAIN)";
        case dns_lookup_error_t::RETRY:
            return "retry (TRY_AGAIN)";
        case dns_lookup_error_t::NODATA:
            return "no data (NO_DATA)";
        case dns_lookup_error_t::PARSE:
            return "dns record parse error";
        default:
            return "unknown error";
    }
}

int get_dns_type(dns_record_t type) {
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

dns_lookup_error_t get_dns_lookup_err(int error) {
    switch (error) {
        case HOST_NOT_FOUND:
            return dns_lookup_error_t::NX_DOMAIN;
        case NO_DATA:
        case NETDB_SUCCESS:
            return dns_lookup_error_t::NODATA;
        case TRY_AGAIN:
            return dns_lookup_error_t::RETRY;
        default:
            return dns_lookup_error_t::UNKNOWN;
    }
}
