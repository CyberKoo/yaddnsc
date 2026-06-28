//
// Created by Kotarou on 2026/6/17.
//
#include "dns_resolver.h"

#include <arpa/inet.h>
#include <arpa/nameser.h>

#include <netdb.h>
#include <netinet/in.h>
#include <resolv.h>
#include <sys/socket.h>

#include <memory>
#include <optional>

#include <spdlog/spdlog.h>

#include "dns.h"
#include "fmt.hpp"
#include "config_cmake.h"
#include "mixin.h"
#include "network/ip_util.h"
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

        ResolverContext() {
            res_ninit(&state);
        }

        ~ResolverContext() { nclose_or_ndestroy(&state); }

        int query(const char *name, int type, unsigned char *buf, int len) {
            return res_nquery(&state, name, ns_c_in, type, buf, len);
        }

        void set_nameserver(const dns_server &server) {
            if (IPUtil::is_ipv4_address(server.address)) {
                state.nscount = 1;
                state.nsaddr_list[0].sin_family = AF_INET;
                state.nsaddr_list[0].sin_addr.s_addr = inet_addr(server.address.c_str());
                state.nsaddr_list[0].sin_port = htons(server.port);
            } else {
                set_ipv6_nameserver(server);
            }
        }

    private:
        [[maybe_unused, no_unique_address]] NoCopy _nc_;
        [[maybe_unused, no_unique_address]] NoMove _nm_;

        static void nclose_or_ndestroy(struct __res_state *s) {
#if defined(HAVE_RES_NDESTROY)
            res_ndestroy(s);
#else
            res_nclose(s);
#endif
        }

        void set_ipv6_nameserver(const dns_server &server) {
            // zero out settings from resolv.conf
            for (int i = 0; i < state.nscount; ++i) {
                state.nsaddr_list[i] = {};
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
            inet_pton(AF_INET6, server.address.c_str(), &sa6->sin6_addr);
#elif defined(HAVE_RES_SETSERVERS)  // BSD/macOS
            res_sockaddr_union sau{};
            sau.sin6.sin6_port = htons(server.port);
            sau.sin6.sin6_family = AF_INET6;
            inet_pton(AF_INET6, server.address.c_str(), &sau.sin6.sin6_addr);
            res_setservers(&state, &sau, 1);
#endif
        }

#else   // !HAVE_RES_NQUERY — old-style non-reentrant resolver API
        int query(const char *name, int type, unsigned char *buf, int len) {
            static std::mutex res_mutex;
            std::lock_guard lock(res_mutex);
            return res_query(name, ns_c_in, type, buf, len);
        }

        void set_nameserver([[maybe_unused]] const dns_server &server) {
            // Custom DNS server not supported without res_ninit
        }
#endif
    };

    dns_error get_dns_lookup_err(int error) {
        switch (error) {
            case HOST_NOT_FOUND:
                return dns_error::NX_DOMAIN;
            case NO_DATA:
                return dns_error::NODATA;
            case NETDB_SUCCESS:
                return dns_error::UNKNOWN;
            case TRY_AGAIN:
                return dns_error::RETRY;
            default:
                return dns_error::UNKNOWN;
        }
    }

    // ── Single query execution against one nameserver ──
    std::vector<uint8_t> do_query(ResolverContext &ctx, const std::string &host_str, int ns_type) {
        static constexpr int MAXIMUM_UDP_SIZE = 512;

        int buffer_size = MAXIMUM_UDP_SIZE;
        int received_size = 0;
        std::vector<uint8_t> buffer;

        do {
            buffer_size = received_size + buffer_size;
            buffer.resize(buffer_size);

            SPDLOG_TRACE(R"(Sending DNS query for "{}" (buffer size: {}))", host_str, buffer_size);
            received_size = ctx.query(host_str.c_str(), ns_type,
                                      buffer.data(), buffer_size);
            if (received_size < 0) {
                auto error_type = get_dns_lookup_err(h_errno);
                SPDLOG_DEBUG(R"(DNS query failed for "{}": {})", host_str, DNS::error_to_str(error_type));
                throw DnsLookupException(
                    fmt::format(
                        R"(DNS lookup failed for domain "{}", error: {})",
                        host_str, DNS::error_to_str(error_type)), error_type
                );
            }
            SPDLOG_TRACE(R"(DNS query received: {} bytes (buffer size: {}))", received_size, buffer_size);
        } while (received_size >= buffer_size);

        buffer.resize(static_cast<size_t>(received_size));
        SPDLOG_TRACE(R"(DNS response for "{}": {} bytes total)", host_str, received_size);
        return buffer;
    }
}

class DnsResolver::Impl {
public:
    explicit Impl(std::optional<dns_server> server)
        : server_(std::move(server)) {
#if !defined(HAVE_RES_NQUERY)
        if (server_.has_value()) {
            SPDLOG_WARN("Custom resolver defined, but res_nquery() is not supported "
                "on this platform; the option will be ignored");
            server_.reset();
        }
#endif
    }

    ~Impl() = default;

    std::vector<uint8_t> query(const std::string &host_str, dns_type type) const {
        SPDLOG_TRACE(R"(DNS lookup for "{}")", host_str);

        const auto ns_type = DNS::to_ns_type(type);

        // No custom resolver — use the default system resolver.
        if (!server_.has_value()) {
            SPDLOG_TRACE(R"(Using default system resolver for "{}")", host_str);
            ResolverContext ctx;
            return do_query(ctx, host_str, ns_type);
        }

        // Use the single custom resolver.
        SPDLOG_TRACE(R"(Resolving "{}" via custom resolver)", host_str);
        ResolverContext ctx;
        ctx.set_nameserver(*server_);
        return do_query(ctx, host_str, ns_type);
    }

private:
    std::optional<dns_server> server_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

DnsResolver::DnsResolver() : DnsResolver(std::optional<dns_server>{}) {
}

DnsResolver::DnsResolver(std::optional<dns_server> server)
    : impl_(std::make_unique<Impl>(std::move(server))) {
}

DnsResolver::~DnsResolver() = default;

std::vector<uint8_t> DnsResolver::query(const std::string &host, dns_type type) const {
    return impl_->query(host, type);
}
