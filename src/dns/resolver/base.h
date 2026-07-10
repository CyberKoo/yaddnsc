//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_BASE_H
#define YADDNSC_DNS_BASE_H

#include <atomic>
#include <expected>
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

#include "mixin.h"
#include "record_kind.h"
#include "dns/dns_error_info.h"

/// ResolverBase — common interface for all DNS resolvers.
///
/// Provides the shared contract (query + non-copyable semantics) so that
/// callers can work with any resolver type polymorphically.  Each resolver
/// carries a stable numeric id (auto-incremented from a static atomic
/// counter) for unambiguous log references.
class ResolverBase {
public:
    virtual ~ResolverBase() = default;

    ResolverBase(ResolverBase &&) noexcept = default;

    ResolverBase &operator=(ResolverBase &&) noexcept = default;

    /// Perform a DNS query and return the raw response packet.
    ///
    /// @param host      Hostname to look up.
    /// @param type      Record type (A, AAAA, etc.).
    /// @param cancel_fd Optional fd to monitor for cancellation.
    ///                  Pass -1 (default) for no cancellation support.
    ///
    /// @note cancel_fd is supported by all resolvers: DohResolver, DotResolver,
    ///       and ClassicResolver all monitor the fd and abort on cancellation.
    ///
    /// @return  Raw DNS response packet bytes on success, or a DnsErrorInfo
    ///          describing the failure (transport error, NXDOMAIN, timeout, etc.).
    ///          Callers must check the error code via error().code to
    ///          distinguish transient errors (RETRY, CONNECTION) from permanent
    ///          ones (NX_DOMAIN, NODATA).
    [[nodiscard]] virtual std::expected<std::vector<std::uint8_t>, DnsErrorInfo>
    query(const std::string &host, RecordKind type, int cancel_fd = -1) const = 0;

    /// Return a human-readable resolver type name (e.g. "Classic", "DNS-Over-HTTPS").
    [[nodiscard]] virtual std::string_view get_type() const noexcept = 0;

    /// Return the stable unique identifier for this resolver instance.
    [[nodiscard]] std::uint64_t get_id() const noexcept { return id_; }

protected:
    /// Construct with a new auto-incremented ID.
    ResolverBase() : id_(next_id_.fetch_add(1, std::memory_order_relaxed)) {
    }

private:
    std::uint64_t id_;

    inline static std::atomic<std::uint64_t> next_id_{0};

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
};

#endif // YADDNSC_DNS_BASE_H
