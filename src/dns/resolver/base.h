//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_BASE_H
#define YADDNSC_DNS_BASE_H

#include <atomic>
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>

#include "mixin.h"
#include "dns_type.h"

/// ResolverBase — common interface for all DNS resolvers.
///
/// Provides the shared contract (query + non-copyable semantics) so that
/// callers can work with any resolver type polymorphically.  Each resolver
/// carries a stable numeric id (auto-incremented from a static atomic
/// counter) for unambiguous log references.
class ResolverBase {
public:
    /// Tag type for anonymous/temporary resolvers.
    ///
    /// Resolvers constructed with this tag get id_ = 0 instead of consuming an
    /// increment from the global counter. Used for the fallback system resolver
    /// in ClassicResolver's default constructor (a single-use query helper).
    struct AnonymousIdTag {
    };

    virtual ~ResolverBase() = default;

    ResolverBase(ResolverBase &&) noexcept = default;

    ResolverBase &operator=(ResolverBase &&) noexcept = default;

    /// Perform a DNS query and return the raw response packet.
    /// @param host  Hostname to look up.
    /// @param type  Record type (A, AAAA, etc.).
    /// @return      Raw DNS response packet bytes (wire format).
    [[nodiscard]] virtual std::vector<std::uint8_t> query(const std::string &host, DNS::Type type) const = 0;

    /// Return a human-readable resolver type name (e.g. "Classic", "DNS-Over-HTTPS").
    [[nodiscard]] virtual std::string_view get_type() const noexcept = 0;

    /// Return the stable unique identifier for this resolver instance.
    [[nodiscard]] std::uint64_t get_id() const noexcept { return id_; }

protected:
    /// Construct with a new auto-incremented ID.
    ResolverBase() : id_(next_id_.fetch_add(1, std::memory_order_relaxed)) {
    }

    /// Construct an anonymous resolver with id_ = 0.
    explicit ResolverBase(AnonymousIdTag) : id_(0) {
    }

private:
    std::uint64_t id_;

    inline static std::atomic<std::uint64_t> next_id_{0};

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
};

#endif // YADDNSC_DNS_BASE_H
