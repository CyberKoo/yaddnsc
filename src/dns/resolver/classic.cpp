//
// Created by Kotarou on 2026/6/17.
//
#include "classic.h"

#include <netdb.h>
#include <resolv.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/nameser.h>

#include <memory>
#include <mutex>
#include <optional>

#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "mixin.h"
#include "dns/util.h"
#include "config_cmake.h"
#include "network/inet_address.h"
#include "exception/dns_lookup.h"

// only for musl
#ifndef NETDB_INTERNAL
#define NETDB_INTERNAL -1
#endif

#ifndef NETDB_SUCCESS
#define NETDB_SUCCESS 0
#endif

namespace {
    constexpr int MAXIMUM_UDP_SIZE = 512;

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

        ~ResolverContext() { destroy(&state); }

        int query(const char *name, int type, unsigned char *buf, int len) {
            return res_nquery(&state, name, ns_c_in, type, buf, len);
        }

        void set_nameserver(const DNS::Server &server) {
            if (Inet4Address::parse(server.address)) {
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

        static void destroy(struct __res_state *s) {
#if defined(HAVE_RES_NDESTROY)
            res_ndestroy(s);
#else
            res_nclose(s);
#endif
        }

        void set_ipv6_nameserver(const DNS::Server &server) {
            // zero out settings from resolv.conf
            for (int i = 0; i < state.nscount; ++i) {
                state.nsaddr_list[i] = {};
            }

            state.nscount = 1;
            state._u._ext.nscount = 1;

#if defined(HAVE_RES_STATE_EXT_NSADDRS)  // glibc
            // Release any previously allocated IPv6 address structures to prevent memory leaks.
            // res_nclose() frees the internal nsaddrs array but keeps the state alive.
            res_nclose(&state);
            // Re-set counters because res_nclose() zeroed them.
            state.nscount = 1;
            state._u._ext.nscount = 1;
            state._u._ext.nscount6 = 1;
            state._u._ext.nsmap[0] = MAXNS + 1;
            auto *sa6 = state._u._ext.nsaddrs[0];
            if (sa6 == nullptr) {
                // Memory allocated here will be freed in res_nclose() later.
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

        void set_nameserver([[maybe_unused]] const DNS::Server &server) {
            // Custom DNS server not supported without res_ninit
        }
#endif
    };

    DNS::Error get_dns_lookup_err(int error) {
        switch (error) {
            case HOST_NOT_FOUND:
                return DNS::Error::NX_DOMAIN;
            case NO_DATA:
                return DNS::Error::NODATA;
            case NETDB_SUCCESS:
                return DNS::Error::UNKNOWN;
            case TRY_AGAIN:
                return DNS::Error::RETRY;
            default:
                return DNS::Error::UNKNOWN;
        }
    }

    // ── Single query execution against one nameserver ──
    std::vector<uint8_t> do_query(ResolverContext &ctx, const std::string &host_str, int ns_type) {
        int buffer_size = MAXIMUM_UDP_SIZE;
        int received_size = 0;
        std::vector<uint8_t> buffer;

        do {
            buffer_size = received_size + buffer_size;
            buffer.resize(buffer_size);

            SPDLOG_TRACE(R"(Sending DNS query for "{}" (buffer size: {}))", host_str, buffer_size);
            received_size = ctx.query(host_str.c_str(), ns_type, buffer.data(), buffer_size);
            if (received_size < 0) {
                auto error_type = get_dns_lookup_err(h_errno);
                SPDLOG_DEBUG(R"(DNS query failed for "{}": {})", host_str, DNS::error_to_str(error_type));
                throw DnsLookupException(
                    fmt::format(R"(DNS lookup failed for domain "{}", error: {})", host_str,
                                DNS::error_to_str(error_type)), error_type
                );
            }
            SPDLOG_TRACE(R"(DNS query received: {} bytes (buffer size: {}))", received_size, buffer_size);
        } while (received_size >= buffer_size);

        buffer.resize(static_cast<size_t>(received_size));
        SPDLOG_TRACE(R"(DNS response for "{}": {} bytes total)", host_str, received_size);
        return buffer;
    }
}

class ClassicResolver::Impl {
public:
    explicit Impl(std::optional<DNS::Server> server, uint64_t id) : id_(id), server_(std::move(server)) {
#if !defined(HAVE_RES_NQUERY)
        if (server_.has_value()) {
            static std::once_flag flag;
            std::call_once(flag, [&] {
                SPDLOG_WARN("A custom resolver was configured, but res_nquery() is not available "
                    "on this platform. The setting will be ignored. "
                    "Consider using a DoH/DoT resolver instead.");
            });
            server_.reset();
        }
#endif
    }

    ~Impl() = default;

    [[nodiscard]] std::vector<uint8_t> query(const std::string &host_str, DNS::Type type) const {
        SPDLOG_TRACE(R"(Resolver #{} DNS lookup for "{}")", id_, host_str);

        const auto ns_type = DNS::to_ns_type(type);

        // No custom resolver — use the default system resolver.
        if (!server_.has_value()) {
            ResolverContext ctx;
            return do_query(ctx, host_str, ns_type);
        }

        // Use the single custom resolver.
        SPDLOG_DEBUG(R"(Resolver #{} Resolving "{}" (type {}))", id_, host_str, ns_type);
        ResolverContext ctx;
        ctx.set_nameserver(*server_);
        return do_query(ctx, host_str, ns_type);
    }

private:
    uint64_t id_;
    std::optional<DNS::Server> server_;

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;
};

ClassicResolver::ClassicResolver()
    : ResolverBase(AnonymousIdTag{}), impl_(std::make_unique<Impl>(std::nullopt, get_id())) {
}

ClassicResolver::ClassicResolver(std::optional<DNS::Server> server)
    : impl_(std::make_unique<Impl>(std::move(server), get_id())) {
}

ClassicResolver::~ClassicResolver() = default;

std::vector<uint8_t> ClassicResolver::query(const std::string &host, DNS::Type type) const {
    return impl_->query(host, type);
}
