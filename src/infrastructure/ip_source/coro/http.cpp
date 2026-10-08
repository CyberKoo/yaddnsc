//
// ip_source — coroutine HTTP source (implementation).
//

#include "http.h"

#include <string>
#include <utility>
#include <vector>

#include "domain/network/inet_address.h"
#include "infrastructure/net/http/client.h"
#include "infrastructure/net/http/error.h"
#include "support/fmt.hpp"
#include "support/string_util.hpp"

namespace ipsource {

HttpIpSource::HttpIpSource(std::string url, AddressFamily address_family, std::string bind_interface,
                           http::Options base_options)
    : url_(std::move(url)), address_family_(address_family), options_(std::move(base_options)) {
    if (address_family_ != AddressFamily::UNSPECIFIED) {
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
        const auto code = response.error().code == http::ErrorCode::CANCELLED
                              ? domain::IpSourceError::Code::CANCELLED
                              : domain::IpSourceError::Code::UNAVAILABLE;
        co_return std::unexpected(domain::IpSourceError{
            code, fmt::format(R"(HTTP IP source "{}" did not return a valid response: {})", url_,
                              response.error().message)});
    }

    auto address = InetAddress::parse(StringUtil::trim(response->text()));
    if (!address.has_value()) {
        co_return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                  fmt::format(R"(HTTP IP source "{}" did not return a valid message)", url_)});
    }
    if (address_family_ != AddressFamily::UNSPECIFIED && address->get_family() != address_family_) {
        co_return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE,
                                  fmt::format(R"(HTTP IP source "{}" returned an address of the wrong family)", url_)});
    }
    co_return std::vector<InetAddress>{*std::move(address)};
}

}  // namespace ipsource
