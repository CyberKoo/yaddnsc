//
// ip_source — coroutine interface source (implementation).
//

#include "iface.h"

#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>  // IWYU pragma: keep — std::erase_if overload for std::vector lives here; clangd sees no spelled use

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

#include <expected>

#include "coro/cancelled.h"
#include "coro/offload.hpp"
#include "domain/error/error.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/ip_source/iface_util.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not

namespace ipsource {

InterfaceIpSource::InterfaceIpSource(std::string interface_name, domain::AddressFamily address_family)
    : interface_name_(std::move(interface_name)), address_family_(address_family) {}

coro::Task<Result> InterfaceIpSource::resolve() {
    try {
        auto addresses = co_await coro::offload([name = interface_name_] { return ipsource::get_addresses(name); });
        if (!addresses.has_value()) {
            co_return std::unexpected(domain::IpSourceError{
                domain::IpSourceError::Code::UNAVAILABLE, fmt::format(R"(interface "{}" not found)", interface_name_)});
        }

        if (address_family_ != domain::AddressFamily::UNSPECIFIED) {
            std::erase_if(*addresses,
                          [af = address_family_](const domain::InetAddress& addr) { return addr.get_family() != af; });
        }
        co_return std::move(*addresses);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const coro::Cancelled&) {
        throw;
    } catch (const std::exception& error) {
        co_return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE, error.what()});
    } catch (...) {
        co_return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, "unknown interface source exception"});
    }
}

}  // namespace ipsource
