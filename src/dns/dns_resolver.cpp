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
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <optional>
#include <config_cmake.h>

#include "dns.h"
#include "fmt.hpp"
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

    dns_error get_dns_lookup_err(int error) {
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

    // ── Single query execution against one nameserver ──
    std::vector<uint8_t> do_query(ResolverContext &ctx,
                                  const std::string &host_str,
                                  int ns_type) {
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

    bool should_fallback(dns_error error) {
        return error == dns_error::RETRY || error == dns_error::UNKNOWN;
    }

    // ── Shared state for concurrent queries (heap-allocated, outlives caller) ──
    struct ConcurrentState {
        std::mutex mtx;
        std::condition_variable cv;
        std::vector<uint8_t> result;
        bool has_result = false;
        std::optional<DnsLookupException> definitive_error;
        std::optional<DnsLookupException> transient_error;
        int completed = 0;
        int total = 0;
    };
}

class DnsResolver::Impl {
public:
    explicit Impl(std::vector<DnsServer> servers)
        : servers_(std::move(servers)) {
#if !defined(HAVE_RES_NQUERY)
        if (!servers_.empty()) {
            SPDLOG_WARN("Custom resolver defined, but res_nquery() is not supported "
                         "on this platform; the option will be ignored");
            servers_.clear();
        }
#endif
    }

    ~Impl() = default;

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;
    Impl(Impl &&) = delete;
    Impl &operator=(Impl &&) = delete;

    // ── Concurrent query: fire all resolvers in parallel, take fastest ──
    std::vector<uint8_t> query_concurrent(const std::string &host_str, int ns_type) {
        auto state = std::make_shared<ConcurrentState>();
        state->total = static_cast<int>(servers_.size());

        for (size_t i = 0; i < servers_.size(); ++i) {
            auto addr = servers_[i].ip_address;
            auto port = servers_[i].port;

            SPDLOG_TRACE(R"(Launching concurrent resolver #{} ({}:{}) for "{}")",
                         i, addr, port, host_str);

            // Each thread creates its own ResolverContext on the stack,
            // configures its nameserver, and runs the query.
            // No shared state between threads — the context is RAII and
            // gets cleaned up when the thread function returns.
            std::thread(
                [host = host_str,
                 ns_type,
                 addr = std::move(addr),
                 port,
                 state]() {
                    ResolverContext ctx;
                    ctx.set_nameserver(DnsServer{addr, port});

                    try {
                        auto data = do_query(ctx, host, ns_type);
                        std::lock_guard lock(state->mtx);
                        if (!state->has_result) {
                            SPDLOG_TRACE(R"(Resolver {}:{} responded first for "{}")",
                                         addr, port, host);
                            state->result = std::move(data);
                            state->has_result = true;
                            state->cv.notify_one();
                        } else {
                            SPDLOG_TRACE(R"(Resolver {}:{} also succeeded for "{}", discarded)",
                                         addr, port, host);
                        }
                    } catch (const DnsLookupException &e) {
                        std::lock_guard lock(state->mtx);
                        if (should_fallback(e.get_error())) {
                            SPDLOG_TRACE(R"(Resolver {}:{} failed for "{}" (transient: {}))",
                                         addr, port, host, DNS::error_to_str(e.get_error()));
                            state->transient_error = e;
                        } else if (!state->definitive_error.has_value()) {
                            SPDLOG_DEBUG(R"(Resolver {}:{} returned {} for "{}")",
                                         addr, port, DNS::error_to_str(e.get_error()), host);
                            state->definitive_error = e;
                        }
                        ++state->completed;
                        if (state->completed == state->total) {
                            state->cv.notify_one();
                        }
                    }
                }).detach();
        }

        // Wait for first success, or all threads to finish.
        {
            std::unique_lock lock(state->mtx);
            state->cv.wait(lock, [&state] {
                return state->has_result || state->completed == state->total;
            });
        }

        if (state->has_result) {
            return std::move(state->result);
        }

        // All failed — throw the best error we have.
        if (state->definitive_error.has_value()) {
            throw state->definitive_error.value();
        }

        auto last_err = state->transient_error.value_or(
            DnsLookupException("All resolvers failed", dns_error::UNKNOWN));

        if (state->total > 1) {
            SPDLOG_ERROR(
                R"(All {} resolver(s) failed for domain "{}", last error: {})",
                state->total, host_str,
                DNS::error_to_str(last_err.get_error()));
        }

        throw last_err;
    }

    std::vector<uint8_t> query(const std::string &host_str, dns_type type) {
        SPDLOG_DEBUG(R"(DNS lookup for domain "{}")", host_str);

        const auto ns_type = to_ns_type(type);

        // No custom resolvers — use the default system resolver.
        if (servers_.empty()) {
            SPDLOG_TRACE(R"(Using default system resolver for "{}")", host_str);
            ResolverContext ctx;
            return do_query(ctx, host_str, ns_type);
        }

        // Single resolver — no concurrency overhead needed.
        if (servers_.size() == 1) {
            SPDLOG_TRACE(R"(Using single custom resolver {}:{} for "{}")",
                         servers_[0].ip_address, servers_[0].port, host_str);
            ResolverContext ctx;
            ctx.set_nameserver(servers_[0]);
            return do_query(ctx, host_str, ns_type);
        }

        // Multiple resolvers — fire all queries concurrently via detached threads.
        // Each thread creates its own ephemeral ResolverContext, so no server-side
        // state or shared_ptr overhead is needed.
        SPDLOG_TRACE(R"(Firing {} resolver(s) concurrently for "{}")", servers_.size(), host_str);
        return query_concurrent(host_str, ns_type);
    }

private:
    [[nodiscard]] static int to_ns_type(dns_type type) noexcept {
        switch (type) {
            case dns_type::A: return ns_t_a;
            case dns_type::AAAA: return ns_t_aaaa;
            case dns_type::TXT: return ns_t_txt;
            case dns_type::SOA: return ns_t_soa;
            default: return ns_t_invalid;
        }
    }

private:
    std::vector<DnsServer> servers_;
};

DnsResolver::DnsResolver() : DnsResolver(std::vector<DnsServer>{}) {
}

DnsResolver::DnsResolver(std::vector<DnsServer> servers)
    : impl_(std::make_unique<Impl>(std::move(servers))) {
}

DnsResolver::~DnsResolver() = default;

std::vector<uint8_t> DnsResolver::query(const std::string &host, dns_type type) const {
    return impl_->query(host, type);
}
