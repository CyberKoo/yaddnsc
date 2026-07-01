//
// Created by Kotarou on 2026/6/28.
//

#ifndef YADDNSC_DNS_BASE_H
#define YADDNSC_DNS_BASE_H

#include <atomic>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

#include "type.h"
#include "mixin.h"

// ── Tag type for anonymous/temporary resolvers ──
//
// Resolvers constructed with this tag get id_ = 0 instead of consuming an
// increment from the global counter. Used for the fallback system resolver
// in ClassicResolver's default constructor (a single-use query helper).
struct AnonymousIdTag {
};

// ---------------------------------------------------------------------------
// ResolverBase — common interface for all DNS resolvers.
//
// Provides the shared contract (query + non-copyable semantics) so that
// callers can work with any resolver type polymorphically.  Each resolver
// carries a stable numeric id (auto-incremented from a static atomic
// counter) for unambiguous log references.
// ---------------------------------------------------------------------------
class ResolverBase {
public:
    virtual ~ResolverBase() = default;

    ResolverBase(ResolverBase &&) noexcept = default;

    ResolverBase &operator=(ResolverBase &&) noexcept = default;

    [[nodiscard]] virtual std::vector<uint8_t> query(const std::string &host, dns_type type) const = 0;

    [[nodiscard]] virtual std::string_view get_type() const noexcept = 0;

    [[nodiscard]] uint64_t get_id() const noexcept { return id_; }

protected:
    ResolverBase() : id_(next_id_.fetch_add(1, std::memory_order_relaxed)) {
    }

    // For anonymous/temporary resolvers that should not consume an ID from the
    // global counter (e.g. the fallback system resolver in MultiResolver).
    explicit ResolverBase(AnonymousIdTag) : id_(0) {
    }

private:
    uint64_t id_;

    inline static std::atomic<uint64_t> next_id_{0};

    [[maybe_unused, no_unique_address]] NoCopy _nc_;
};

#endif // YADDNSC_DNS_BASE_H
