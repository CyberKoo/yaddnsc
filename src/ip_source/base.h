//
// Created by Kotarou on 2026/7/1.
//

#ifndef YADDNSC_IP_SOURCE_BASE_H
#define YADDNSC_IP_SOURCE_BASE_H

#include <vector>

#include "network/inet_address.h"

#include "mixin.h"

/// IpSourceBase — abstract interface for obtaining a local IP address.
///
/// Three concrete implementations exist:
///   - InterfaceIpSource — reads addresses from a local network interface
///   - HttpIpSource      — fetches the address from an external HTTP service
///   - MdnsIpSource      — discovers a LAN device via mDNS multicast
///
/// @section exception-contract Exception contract
///
/// All implementations throw on failure — the exception aborts the current
/// resolve() operation and propagates up the call stack.  This is a deliberate
/// design choice over returning std::expected:
///
///   • The caller (Updater) treats all IP source failures uniformly as "skip
///     this update and retry on the next cycle".  It does not distinguish
///     error types or attempt fallback logic.
///   • Exceptions are caught only at the noexcept boundary in
///     Updater::process(), which logs the error and continues with the next
///     task.  No retry, no fallback, no branching on error type in the catch
///     block.
///   • This avoids coupling the caller to per-source error types while still
///     preserving diagnostic information via the exception message.
///
/// The one outlier is Factory::create(), which uses std::unreachable() rather
/// than throw after an exhaustive switch over Config::IpSource — an "unreachable"
/// IP source type is a compile-time invariant violation, not a runtime condition.
///
/// @note Thread-safe: resolve() is const and does not mutate shared state.
class IpSourceBase {
public:
    virtual ~IpSourceBase() = default;

    IpSourceBase() = default;

    IpSourceBase(IpSourceBase &&) noexcept = default;

    IpSourceBase &operator=(IpSourceBase &&) noexcept = default;

    /// Resolve the local IP address(es).
    ///
    /// For sources that return multiple candidates (interface, mDNS), all found
    /// addresses are returned so the caller can apply policy filters.
    ///
    /// @return  A vector of resolved addresses (maybe empty if the source has
    ///          no addresses of the requested family).
    ///
    /// @throws std::runtime_error  If the source cannot determine its IP address
    ///         at all (network error, interface not found, parse failure, etc.).
    ///         An empty return means the source succeeded but found no matching
    ///         addresses — distinct from a failure to reach the source.
    [[nodiscard]] virtual std::vector<InetAddress> resolve() const = 0;

private:
    [[maybe_unused, no_unique_address]] NoCopy _nc_;
};

#endif  // YADDNSC_IP_SOURCE_BASE_H
