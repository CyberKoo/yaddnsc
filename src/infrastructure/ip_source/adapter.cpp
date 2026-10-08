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
#include "infrastructure/ip_source/http.h"
#include "infrastructure/ip_source/iface.h"
#include "infrastructure/ip_source/mdns.h"
#include "infrastructure/ip_source/source.h"

namespace ipsource {

namespace {

/// Address family implied by the record type (A → IPv4, AAAA → IPv6).
[[nodiscard]] constexpr AddressFamily type_to_family(RecordKind type) noexcept {
    switch (type) {
        case RecordKind::A:
            return AddressFamily::IPV4;
        case RecordKind::AAAA:
            return AddressFamily::IPV6;
        default:
            return AddressFamily::UNSPECIFIED;
    }
}

}  // namespace

IpSourceAdapter::IpSourceAdapter(http::Options http_options) : options_(std::move(http_options)) {}

coro::Task<std::expected<std::vector<InetAddress>, domain::IpSourceError>> IpSourceAdapter::resolve(
    const domain::SubdomainConfig& config) {
    try {
        const auto family = type_to_family(config.type);
        switch (config.ip_source) {
            case Config::IpSource::INTERFACE: {
                InterfaceIpSource source{config.interface, family};
                co_return co_await source.resolve();
            }
            case Config::IpSource::HTTP: {
                HttpIpSource source{config.ip_source_param, family, config.interface, options_};
                co_return co_await source.resolve();
            }
            case Config::IpSource::MDNS: {
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
    } catch (...) {
        co_return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, "unknown non-standard exception"});
    }
}

}  // namespace ipsource
