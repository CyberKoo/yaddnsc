//
// ip_source — coroutine HTTP source (implementation).
//

#include "http.h"

#include <coroutine>  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "domain/error/error.h"
#include "domain/network/address_family.h"
#include "domain/network/inet_address.h"
#include "infrastructure/http/client.h"
#include "infrastructure/http/error.h"
#include "infrastructure/network/transport/options.h"
#include "support/string_util.hpp"  // IWYU pragma: keep — the StringUtil namespace alias lives here
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "yaddnsc/util/string_util.hpp"

namespace ipsource {

HttpIpSource::HttpIpSource(std::string url, domain::AddressFamily address_family, std::string bind_interface,
                           http::Options base_options)
    : url_(std::move(url)), address_family_(address_family), options_(std::move(base_options)) {
    if (address_family_ != domain::AddressFamily::UNSPECIFIED) {
        options_.address_family = address_family_;
    }
    if (!bind_interface.empty()) {
        options_.connect.interface = std::move(bind_interface);
    }
}

coro::Task<Result> HttpIpSource::resolve() {
    http::Request request;
    request.method = http::Method::GET;

    http::Client client{options_};
    auto response = co_await client.exchange(url_, request);
    if (!response) {
        // The transport message (which names the host) stays at debug level;
        // the cycle log gets a short reason phrase naming the server by role.
        // The URL is redacted: its query can carry credentials.
        SPDLOG_DEBUG(R"(HTTP IP source "{}" request failed: {})", http::redacted_url(url_), response.error().message);
        const auto code = domain::IpSourceError::Code::UNAVAILABLE;
        co_return std::unexpected(
            domain::IpSourceError{code, http::failure_reason("IP source server", response.error())});
    }

    auto address = domain::InetAddress::parse(StringUtil::trim(response->text()));
    if (!address.has_value()) {
        co_return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                                        "the IP source server did not return a valid IP address"});
    }
    if (address_family_ != domain::AddressFamily::UNSPECIFIED && address->get_family() != address_family_) {
        co_return std::unexpected(domain::IpSourceError{
            domain::IpSourceError::Code::UNAVAILABLE, "the IP source server returned an address of the wrong family"});
    }
    co_return std::vector<domain::InetAddress>{*address};
}

}  // namespace ipsource
