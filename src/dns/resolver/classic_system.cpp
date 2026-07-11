//
// Created by Kotarou on 2026/6/17.
//
// ── Classic system resolver (libresolv) ──
//
// ⚠  This is the legacy system resolver based on libresolv.  It is
//    superseded by the native resolver (classic_native, no libresolv
//    dependency).
//
//    This file is in maintenance-only mode:
//      • Only compilation fixes and bug fixes will be applied here.
//      • No new features, improvements, or refactoring will be added.
//      • New functionality should go to the native resolver.
//
//    This file will be removed once the native resolver is stable.
//
//
//
#include <memory>
#include <mutex>
#include <optional>

#include "dns/resolver_registry.h"
#include "dns/util.hpp"
#include "exception/dns_lookup.h"
#include "network/inet_address.h"

#include "classic.h"
#include "dns/dns_error_info.h"
#include "config_cmake.h"
#include "dns_error.h"
#include "mixin.h"
#include "uri.h"

#include "fmt.hpp"
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include <netinet/in.h>
#include <resolv.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>

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
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc) — glibc res_state internals require raw
        // C memory
        return static_cast<T *>(calloc(count, sizeof(T)));
    }

    // ── RAII wrapper that isolates all resolver platform branching ──
    struct ResolverContext {
#ifdef HAVE_RES_NQUERY
        struct __res_state state{};

        ResolverContext() {
            res_ninit(&state);
            // Set 1-second per-attempt timeout with 1 retry (2 attempts
            // total), for a maximum total timeout of ~2 seconds.
            // retry=0 leads to immediate failure when the first UDP packet
            // is dropped; at least 1 retry is needed for reliability.
            state.retrans = 1;
            state.retry = 1;
        }

        ~ResolverContext() {
            destroy(&state);
        }

        int query(const char *name, int type, unsigned char *buf, int len) {
            return res_nquery(&state, name, ns_c_in, type, buf, len);
        }

        void set_nameserver(const Config::DnsServer &server) {
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
        [[maybe_unused, no_unique_address]] NoCopy no_copy_;
        [[maybe_unused, no_unique_address]] NoMove no_move_;

        static void destroy(struct __res_state *s) {
#if defined(HAVE_RES_NDESTROY)
            res_ndestroy(s);
#else
            res_nclose(s);
#endif
        }

        void set_ipv6_nameserver(const Config::DnsServer &server) {
            // zero out settings from resolv.conf
            for (int i = 0; i < state.nscount; ++i) {
                state.nsaddr_list[i] = {};
            }

            state.nscount = 1;
            state._u._ext.nscount = 1;

#if defined(HAVE_RES_STATE_EXT_NSADDRS)  // glibc
            // Release any previously allocated IPv6 address structures to prevent memory leaks.
            // Manually free each entry instead of calling res_nclose() — nsaddrs may be an
            // inline array (glibc ≥ 2.34) or a pointer (older glibc); manual per-entry
            // cleanup works for both layouts.
            // NOLINTBEGIN(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc,modernize-loop-convert)
            // — 'nsaddr' is a macro from resolv.h expanding to 'nsaddr_list[0]', range-for impossible
            for (int i = 0; i < MAXNS; ++i) {
                // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
                free(state._u._ext.nsaddrs[i]);
                state._u._ext.nsaddrs[i] = nullptr;
            }
            // NOLINTEND(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc,modernize-loop-convert)

            state.nscount = 1;
            state._u._ext.nscount = 1;
            state._u._ext.nscount6 = 1;
            state._u._ext.nsmap[0] = MAXNS + 1;

            auto *sa6 = ccalloc<struct sockaddr_in6>(1);
            if (sa6 == nullptr) {
                throw std::bad_alloc();
            }
            state._u._ext.nsaddrs[0] = sa6;

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

#else  // !HAVE_RES_NQUERY — old-style non-reentrant resolver API
        int query(const char *name, int type, unsigned char *buf, int len) {
            static std::mutex res_mutex;
            std::lock_guard lock(res_mutex);
            return res_query(name, ns_c_in, type, buf, len);
        }

        void set_nameserver([[maybe_unused]] const Config::DnsServer &server) {
            // Custom DNS server not supported without res_ninit
        }
#endif
    };

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static) — free function in anonymous namespace, false positive
    DnsError get_dns_lookup_err(int error) {
        switch (error) {
            case HOST_NOT_FOUND:
                return DnsError::NX_DOMAIN;
            case NO_DATA:
                return DnsError::NODATA;
            case NETDB_SUCCESS:
                return DnsError::UNKNOWN;
            case TRY_AGAIN:
                return DnsError::RETRY;
            default:
                return DnsError::UNKNOWN;
        }
    }

    // ── Single query execution against one nameserver ──
    std::vector<std::uint8_t> do_query(ResolverContext &ctx, const std::string &host_str, int ns_type,
                                       std::uint64_t resolver_id) {
        int buffer_size = MAXIMUM_UDP_SIZE;
        int received_size = 0;
        std::vector<std::uint8_t> buffer;

        do {
            buffer_size = received_size + buffer_size;
            buffer.resize(static_cast<size_t>(buffer_size));

            SPDLOG_TRACE(R"(Sending DNS query for "{}" (buffer size: {}))", host_str, buffer_size);
            received_size = ctx.query(host_str.c_str(), ns_type, buffer.data(), buffer_size);
            if (received_size < 0) {
                auto error_type = get_dns_lookup_err(h_errno);
                SPDLOG_DEBUG(R"(Resolver #{} DNS query failed for "{}": {})", resolver_id, host_str,
                             error_to_str(error_type));
                throw DnsLookupException(
                    fmt::format(R"(DNS lookup failed for domain "{}", error: {})", host_str, error_to_str(error_type)),
                    error_type);
            }
            SPDLOG_TRACE(R"(DNS query received: {} bytes (buffer size: {}))", received_size, buffer_size);
        } while (received_size >= buffer_size);

        buffer.resize(static_cast<size_t>(received_size));
        SPDLOG_TRACE(R"(DNS response for "{}": {} bytes total)", host_str, received_size);
        return buffer;
    }
} // namespace

// ===========================================================================
//  ClassicResolver::Impl  —  private implementation
// ===========================================================================

struct ClassicResolver::Impl {
    explicit Impl(Config::DnsServer server, std::uint64_t id);

    ~Impl() = default;

    [[nodiscard]] std::expected<std::vector<std::uint8_t>, DnsErrorInfo> query(
        const std::string &host_str, RecordKind type, int cancel_fd = -1) const;

    std::uint64_t id_;
    Config::DnsServer server_;
    Uri uri_;
};

ClassicResolver::Impl::Impl(Config::DnsServer server, std::uint64_t id)
    : id_(id), server_(std::move(server)), uri_(Uri::parse(server_.address)) {
}

std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
ClassicResolver::Impl::query(const std::string &host_str, RecordKind type, [[maybe_unused]] int cancel_fd) const {
    try {
        SPDLOG_TRACE(R"(Resolver #{} DNS lookup for "{}")", id_, host_str);

        // Convert to int for res_nquery (RecordType shares numeric values with ns_t_* per RFC 1035).
        const auto record_type = DNS::Util::type_to_record_type(type);
        const int ns_type_val = static_cast<int>(record_type);

        SPDLOG_DEBUG(R"(Resolver #{} Resolving "{}" (type {}) via {}:{})", id_, host_str, ns_type_val,
                     uri_.get_host_literal(), server_.port);
        ResolverContext ctx;
        ctx.set_nameserver(server_);
        return do_query(ctx, host_str, ns_type_val, id_);
    } catch (const DnsLookupException &e) {
        return std::unexpected(DnsErrorInfo{e.get_error(), e.what()});
    }
}

// ===========================================================================
//  ClassicResolver  —  public API
// ===========================================================================

ClassicResolver::ClassicResolver(Config::DnsServer server) : impl_(
    std::make_unique<Impl>(std::move(server), get_id())) {
}

ClassicResolver::~ClassicResolver() = default;

std::expected<std::vector<std::uint8_t>, DnsErrorInfo> ClassicResolver::query(
    const std::string &host, RecordKind type, int cancel_fd) const {
    return impl_->query(host, type, cancel_fd);
}

// ===========================================================================
//  Self-registration
// ===========================================================================

namespace {
    [[maybe_unused]] DnsResolverRegistry::Registrar _classic(
        "", [](const Config::DnsServer &server) -> std::unique_ptr<ResolverBase> {
            return std::make_unique<ClassicResolver>(server);
        });
} // namespace
