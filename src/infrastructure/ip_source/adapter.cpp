//
// ip_source — the coroutine IP-source factory as an application port
// (implementation).
//

#include "adapter.h"

#include <exception>
#include <new>
#include <string>
#include <utility>

#include "domain/config/ip_source_kind.h"
#include "domain/network/address_family.h"
#include "infrastructure/coro/cancelled.h"
#include "infrastructure/ip_source/http.h"
#include "infrastructure/ip_source/iface.h"
#include "infrastructure/ip_source/mdns.h"
#include "infrastructure/ip_source/source.h"

namespace ipsource {

namespace {

/// Address family implied by the record type (A → IPv4, AAAA → IPv6).
[[nodiscard]] constexpr domain::AddressFamily type_to_family(domain::RecordKind type) noexcept {
    switch (type) {
        case domain::RecordKind::A:
            return domain::AddressFamily::IPV4;
        case domain::RecordKind::AAAA:
            return domain::AddressFamily::IPV6;
        default:
            return domain::AddressFamily::UNSPECIFIED;
    }
}

}  // namespace

IpSourceAdapter::IpSourceAdapter(http::Options http_options) : options_(std::move(http_options)) {}

coro::Task<std::expected<std::vector<domain::InetAddress>, domain::IpSourceError>> IpSourceAdapter::resolve(
    const domain::SubdomainConfig& config) {
    try {
        const auto family = type_to_family(config.type);
        switch (config.ip_source) {
            case domain::IpSource::INTERFACE: {
                InterfaceIpSource source{config.interface, family};
                co_return co_await source.resolve();
            }
            case domain::IpSource::HTTP: {
                HttpIpSource source{config.ip_source_param, family, config.interface, options_};
                co_return co_await source.resolve();
            }
            case domain::IpSource::MDNS: {
                MdnsIpSource source{config.ip_source_param, config.type, config.interface};
                co_return co_await source.resolve();
            }
        }
        co_return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, "unknown IP source kind"});
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& error) {
        co_return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, error.what()});
    } catch (const coro::Cancelled&) {
        throw;
    } catch (...) {
        co_return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, "unknown non-standard exception"});
    }
}

}  // namespace ipsource
