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

#ifndef HAVE_RES_NQUERY
#include <mutex>
#endif

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
    template<typename T>
    T *ccalloc(size_t count) {
        return static_cast<T *>(calloc(count, sizeof(T)));
    }

    // ── RAII wrapper that isolates all resolver platform branching ──
    struct ResolverContext {
#ifdef HAVE_RES_NQUERY
        struct __res_state state{};

        ResolverContext()  { res_ninit(&state); }
        ~ResolverContext() { nclose_or_ndestroy(&state); }

        ResolverContext(const ResolverContext &) = delete;
        ResolverContext &operator=(const ResolverContext &) = delete;

        int query(const char *name, int type, unsigned char *buf, int len) {
            return res_nquery(&state, name, ns_c_in, type, buf, len);
        }

        void set_nameserver(const DnsServer &server) {
            if (IPUtil::is_ipv4_address(server.ip_address)) {
                state.nscount = 1;
                state.nsaddr_list[0].sin_family = AF_INET;
                state.nsaddr_list[0].sin_addr.s_addr = inet_addr(server.ip_address.c_str());
                state.nsaddr_list[0].sin_port = htons(server.port);
            } else {
                set_ipv6_nameserver(server);
            }
        }

    private:
        static void nclose_or_ndestroy(struct __res_state *s) {
#if defined(HAVE_RES_NDESTROY)
            res_ndestroy(s);
#else
            res_nclose(s);
#endif
        }

        void set_ipv6_nameserver(const DnsServer &server) {
            // zero out settings from resolv.conf
            for (int i = 0; i < state.nscount; ++i) {
                std::memset(&state.nsaddr_list[i], 0, sizeof(state.nsaddr_list[i]));
            }

            state.nscount = 1;
            state._u._ext.nscount = 1;

#if defined(HAVE_RES_STATE_EXT_NSADDRS)  // glibc
            state._u._ext.nscount6 = 1;
            state._u._ext.nsmap[0] = MAXNS + 1;
            auto *sa6 = state._u._ext.nsaddrs[0];
            if (sa6 == nullptr) {
                // Memory allocated here will be freed in res_nclose()
                // as we have done res_ninit() above.
                sa6 = ccalloc<sockaddr_in6>(1);
                if (sa6 == nullptr) {
                    throw std::bad_alloc();
                }
                state._u._ext.nsaddrs[0] = sa6;
            }

            sa6->sin6_port = htons(server.port);
            sa6->sin6_family = AF_INET6;
            inet_pton(AF_INET6, server.ip_address.c_str(), &sa6->sin6_addr);
#elif defined(HAVE_RES_SETSERVERS)  // BSD/macOS
            res_sockaddr_union sau{};
            sau.sin6.sin6_port = htons(server.port);
            sau.sin6.sin6_family = AF_INET6;
            inet_pton(AF_INET6, server.ip_address.c_str(), &sau.sin6.sin6_addr);
            res_setservers(&state, &sau, 1);
#endif
        }

#else   // !HAVE_RES_NQUERY — old-style non-reentrant resolver API
        int query(const char *name, int type, unsigned char *buf, int len) {
            static std::mutex res_mutex;
            std::lock_guard lock(res_mutex);
            return res_query(name, ns_c_in, type, buf, len);
        }

        void set_nameserver([[maybe_unused]] const DnsServer &server) {
            // Custom DNS server not supported without res_ninit
        }
#endif
    };

    void log_custom_resolver_once(const DnsServer &server) {
        static bool logged = false;
        if (logged) return;
        logged = true;

#if defined(HAVE_RES_NQUERY)
        if (IPUtil::is_ipv4_address(server.ip_address)) {
            SPDLOG_INFO(R"(Use custom resolver "{}:{}")", server.ip_address, server.port);
        } else if (server.ip_address.front() != '[' && server.ip_address.back() != ']') {
            SPDLOG_INFO(R"(Use custom resolver "[{}]:{}")", server.ip_address, server.port);
        }
#else
        SPDLOG_WARN("Custom resolver defined, but res_nquery() is not supported "
                     "on this platform; the option will be ignored");
#endif
    }
}

class DnsResolver::Impl {
public:
    explicit Impl(std::optional<DnsServer> server) {
        if (server.has_value() && !server->ip_address.empty()) {
            log_custom_resolver_once(*server);
            ctx_.set_nameserver(*server);
        }
    }

    ~Impl() = default;

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    std::vector<uint8_t> query(const std::string &host_str, dns_type type) {
        SPDLOG_DEBUG(R"(DNS lookup for domain "{}")", host_str);

        int buffer_size = MAXIMUM_UDP_SIZE;
        int received_size = 0;
        std::vector<uint8_t> buffer;

        do {
            buffer_size = received_size + buffer_size;
            buffer.resize(buffer_size);

            received_size = ctx_.query(host_str.c_str(), to_ns_type(type),
                                       buffer.data(), buffer_size);
            if (received_size < 0) {
                auto error_type = get_dns_lookup_err(h_errno);
                throw DnsLookupException(
                    fmt::format(
                        R"(DNS lookup failed for domain "{}", error: {})",
                        host_str, DNS::error_to_str(error_type)), error_type
                );
            }
            SPDLOG_DEBUG("Response payload size: {}, buffer size: {}", received_size, buffer_size);
        } while (received_size >= buffer_size);

        buffer.resize(static_cast<size_t>(received_size));
        return buffer;
    }

private:
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

    static int to_ns_type(dns_type type) noexcept {
        switch (type) {
            case dns_type::A: return ns_t_a;
            case dns_type::AAAA: return ns_t_aaaa;
            case dns_type::TXT: return ns_t_txt;
            case dns_type::SOA: return ns_t_soa;
            default: return ns_t_invalid;
        }
    }
private:
    ResolverContext ctx_;

    static constexpr int MAXIMUM_UDP_SIZE = 512;
};

DnsResolver::DnsResolver() : DnsResolver(std::nullopt) {
}

DnsResolver::DnsResolver(std::optional<DnsServer> server) : impl_(std::make_unique<Impl>(std::move(server))) {
}

DnsResolver::~DnsResolver() = default;

std::vector<uint8_t> DnsResolver::query(const std::string &host, dns_type type) const {
    return impl_->query(host, type);
}
