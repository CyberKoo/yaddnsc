//
// Created by Kotarou on 2026/6/17.
//
#include "dns_resolver.h"

#include <spdlog/spdlog.h>

#include <arpa/nameser.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <resolv.h>
#include <netdb.h>

#include <cstring>
#include <config_cmake.h>

#include "dns.h"
#include "fmt.h"
#include "ip_util.h"
#include "exception/dns_lookup_exception.h"

// only for musl
#ifndef NETDB_INTERNAL
#define NETDB_INTERNAL -1
#endif

#ifndef NETDB_SUCCESS
#define NETDB_SUCCESS 0
#endif

namespace {
    constexpr int MAXIMUM_UDP_SIZE = 512;

    int to_ns_type(dns_type type) noexcept {
        switch (type) {
            case dns_type::A: return ns_t_a;
            case dns_type::AAAA: return ns_t_aaaa;
            case dns_type::TXT: return ns_t_txt;
            case dns_type::SOA: return ns_t_soa;
            default: return ns_t_invalid;
        }
    }

    template<typename T>
    T *ccalloc(size_t count) {
        return static_cast<T *>(calloc(count, sizeof(T)));
    }
}

class DnsResolver::Impl {
public:
    explicit Impl([[maybe_unused]] std::optional<dns_server> server) {
#ifdef HAVE_RES_NQUERY
        res_ninit(&state_);

        if (server.has_value() && !server->ip_address.empty()) {
            configure_server(*server);
        }
#endif
    }

    ~Impl() {
#ifdef HAVE_RES_NQUERY
#ifdef HAVE_RES_NDESTROY
        res_ndestroy(&state_);
#else
        res_nclose(&state_);
#endif
#endif
    }

    Impl(const Impl &) = delete;

    Impl &operator=(const Impl &) = delete;

    Impl(Impl &&) = delete;

    Impl &operator=(Impl &&) = delete;

    std::vector<unsigned char> query(const std::string &host_str, dns_type type) {
        SPDLOG_DEBUG(R"(DNS lookup for domain "{}")", host_str);

        int buffer_size = MAXIMUM_UDP_SIZE;
        int received_size = 0;
        std::vector<unsigned char> buffer;

        do {
            buffer_size = received_size + buffer_size;
            buffer.resize(buffer_size);

#ifdef HAVE_RES_NQUERY
            received_size = res_nquery(&state_, host_str.c_str(), ns_c_in, to_ns_type(type),
                                       buffer.data(), buffer_size);
#else
            received_size = res_query(host_str.c_str(), ns_c_in, to_ns_type(type),
                                      buffer.data(), buffer_size);
#endif
            if (received_size < 0) {
                auto error_type = get_dns_lookup_err(h_errno);
                throw DnsLookupException(
                    fmt::format(R"(DNS lookup failed for domain "{}", error: {})", host_str, DNS::error_to_str(error_type)),
                    error_type);
            }
            SPDLOG_DEBUG("Response payload size: {}, buffer size: {}", received_size, buffer_size);
        } while (received_size >= buffer_size);

        buffer.resize(static_cast<size_t>(received_size));
        return buffer;
    }

private:
    void configure_server(const dns_server &server) {
        if (IPUtil::is_ipv4_address(server.ip_address)) {
            state_.nscount = 1;
            state_.nsaddr_list[0].sin_family = AF_INET;
            state_.nsaddr_list[0].sin_addr.s_addr = inet_addr(server.ip_address.c_str());
            state_.nsaddr_list[0].sin_port = htons(server.port);
        } else {
            // zero out settings from resolv.conf
            for (int i = 0; i < state_.nscount; ++i) {
                std::memset(&state_.nsaddr_list[i], 0, sizeof(state_.nsaddr_list[i]));
            }

            state_.nscount = 1;
            state_._u._ext.nscount = 1;

#ifdef HAVE_RES_STATE_EXT_NSADDRS // glibc
            state_._u._ext.nscount6 = 1;
            state_._u._ext.nsmap[0] = MAXNS + 1;
            sockaddr_in6 *sa6 = state_._u._ext.nsaddrs[0];
            if (sa6 == nullptr) {
                // Memory allocated here will be freed in res_nclose()
                // as we have done res_ninit() above.
                sa6 = ccalloc<sockaddr_in6>(1);
                if (sa6 == nullptr) {
                    throw std::bad_alloc();
                }
                state_._u._ext.nsaddrs[0] = sa6;
            }

            sa6->sin6_port = htons(server.port);
            sa6->sin6_family = AF_INET6;
            inet_pton(AF_INET6, server.ip_address.c_str(), &sa6->sin6_addr);
#elif defined(HAVE_RES_SETSERVERS) // bsd-libc (*BSD & MacOS)
            res_sockaddr_union sau{};

            sau.sin6.sin6_port = htons(server.port);
            sau.sin6.sin6_family = AF_INET6;
            inet_pton(AF_INET6, server.ip_address.c_str(), &sau.sin6.sin6_addr);
            res_setservers(&state_, &sau, 1);
#endif
        }
    }

    static dns_error get_dns_lookup_err(int error) {
        switch (error) {
            case HOST_NOT_FOUND:
                return dns_error::NX_DOMAIN;
            case NO_DATA:
            case NETDB_SUCCESS:
                return dns_error::NODATA;
            case TRY_AGAIN:
                return dns_error::RETRY;
            default:
                return dns_error::UNKNOWN;
        }
    }

#ifdef HAVE_RES_NQUERY
    struct __res_state state_{};
#endif
};

DnsResolver::DnsResolver() : DnsResolver(std::nullopt) {
}

DnsResolver::DnsResolver(std::optional<dns_server> server)
    : impl_(std::make_unique<Impl>(std::move(server))) {
}

DnsResolver::~DnsResolver() = default;

std::vector<unsigned char> DnsResolver::query(const std::string &host, dns_type type) const {
    return impl_->query(host, type);
}
