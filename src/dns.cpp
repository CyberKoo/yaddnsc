//
// Created by Kotarou on 2022/4/5.
//

#include "dns.h"

#include <spdlog/spdlog.h>

#include <arpa/nameser.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <resolv.h>
#include <netdb.h>
#include <config_cmake.h>

#include "ip_util.h"
#include "exception/dns_lookup_exception.h"

// only for musl
#ifndef NETDB_INTERNAL
#define NETDB_INTERNAL -1
#endif

#ifndef NETDB_SUCCESS
#define NETDB_SUCCESS 0
#endif

// declaration
constexpr static int MAXIMUM_UDP_SIZE = 512;

dns_lookup_error_type get_dns_lookup_err(int);

int get_dns_type(dns_record_type);

std::basic_string<unsigned char> query(std::string_view, dns_record_type, const std::optional<dns_server> &);

template<typename T>
T *ccalloc(size_t);

std::vector<std::string>
DNS::resolve(std::string_view host, dns_record_type type, const std::optional<dns_server> &server) {
    auto query_res = query(host, type, server);

    ns_msg dns_message{};
    ns_initparse(query_res.c_str(), static_cast<int>(query_res.size()), &dns_message);
    auto answers = ns_msg_count(dns_message, ns_s_an);

    std::vector<std::string> resolve_result;
    for (auto i = 0; i < answers; i++) {
        ns_rr dns_resource{};
        if (ns_parserr(&dns_message, ns_s_an, i, &dns_resource)) {
            throw DnsLookupException(fmt::format("An error occurred when parsing DNS resource, detail: {}",
                                                 strerror(errno)), dns_lookup_error_type::PARSE);
        }

        auto dns_type = ns_rr_type(dns_resource);
        char address_buffer[INET6_ADDRSTRLEN] = {};
        if (dns_type == ns_t_a) {
            inet_ntop(AF_INET, ns_rr_rdata(dns_resource), address_buffer, INET6_ADDRSTRLEN);
            resolve_result.emplace_back(address_buffer);
        } else if (dns_type == ns_t_aaaa) {
            inet_ntop(AF_INET6, ns_rr_rdata(dns_resource), address_buffer, INET6_ADDRSTRLEN);
            resolve_result.emplace_back(address_buffer);
        }
    }

    return resolve_result;
}

std::basic_string<unsigned char>
query(std::string_view host, dns_record_type type, [[maybe_unused]]const std::optional<dns_server> &server) {
    int buffer_size = MAXIMUM_UDP_SIZE;

#ifdef HAVE_RES_NQUERY
    SPDLOG_DEBUG(R"(DNS lookup for domain "{}")", host);
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
                sa6 = ccalloc<struct sockaddr_in6>(1);
                local_state._u._ext.nsaddrs[0] = sa6;
            }

            sa6->sin6_port = htons(server->port);
            sa6->sin6_family = AF_INET6;
            inet_pton(AF_INET6, server->ip_address.c_str(), &sa6->sin6_addr);
#elif defined(HAVE_RES_SETSERVERS) // bsd-libc (*BSD & MacOS)
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

    return {buffer.get(), static_cast<unsigned int>(received_size)};
}

std::string_view DNS::error_to_str(dns_lookup_error_type error) {
    switch (error) {
        case dns_lookup_error_type::NX_DOMAIN:
            return "no such domain (NXDOMAIN)";
        case dns_lookup_error_type::RETRY:
            return "retry (TRY_AGAIN)";
        case dns_lookup_error_type::NODATA:
            return "no data (NO_DATA)";
        case dns_lookup_error_type::PARSE:
            return "dns record parse error";
        default:
            return "unknown error";
    }
}

int get_dns_type(dns_record_type type) {
    switch (type) {
        case dns_record_type::A:
            return ns_t_a;
        case dns_record_type::AAAA:
            return ns_t_aaaa;
        case dns_record_type::TXT:
            return ns_t_txt;
        default:
            return ns_t_soa;
    }
}

dns_lookup_error_type get_dns_lookup_err(int error) {
    switch (error) {
        case HOST_NOT_FOUND:
            return dns_lookup_error_type::NX_DOMAIN;
        case NO_DATA:
        case NETDB_SUCCESS:
            return dns_lookup_error_type::NODATA;
        case TRY_AGAIN:
            return dns_lookup_error_type::RETRY;
        default:
            return dns_lookup_error_type::UNKNOWN;
    }
}

template<typename T>
T *ccalloc(size_t count) {
    return static_cast<T *>(calloc(count, sizeof(T)));
}