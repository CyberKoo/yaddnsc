//
// Created by Kotarou on 2026/7/1.
//

#include "http.h"

#include <optional>
#include <stdexcept>
#include <utility>

#include "network/inet_address.h"

#include "string_util.hpp"
#include "uri.h"
#include "version.h"

#include "fmt.hpp"
#include <spdlog/spdlog.h>

namespace {
/// Build the client options for an HTTP IP source.
[[nodiscard]] net::http::Options make_client_options(const AddressFamily address_family,
                                                     const std::string &bind_interface) {
    net::http::Options opts;
    opts.user_agent = YADDNSC::get_full_version();
    if (address_family != AddressFamily::UNSPECIFIED) {
        opts.transport.address_family = address_family;
    }
    if (!bind_interface.empty()) {
        opts.transport.interface = bind_interface;
    }
    return opts;
}
} // namespace

// ===========================================================================
// HttpIpSource — fetch public IP from an external HTTP service.
// ===========================================================================

HttpIpSource::~HttpIpSource() = default;

HttpIpSource::HttpIpSource(std::string url, const AddressFamily address_family, std::string bind_interface,
                           Utils::CancellationToken token)
    : url_(std::move(url)), address_family_(address_family), bind_interface_(std::move(bind_interface)),
      client_(url_, make_client_options(address_family_, bind_interface_), std::move(token)) {
}

// ---------------------------------------------------------------------------
// HttpIpSource::resolve — send GET request and parse the response body as an IP.
// ---------------------------------------------------------------------------

std::vector<InetAddress> HttpIpSource::resolve() const {
    net::http::Request req;
    req.method = net::http::Method::GET;

    auto resp = client_.exchange(url_, req);
    if (!resp) {
        throw std::runtime_error(
            fmt::format(R"(HTTP IP source "{}" did not return a valid response: {})", url_, resp.error().message));
    }

    auto addr = InetAddress::parse(StringUtil::trim(resp->body));
    if (!addr) {
        throw std::runtime_error(fmt::format(R"(HTTP IP source "{}" did not return a valid message)", url_));
    }
    SPDLOG_DEBUG("Resolved IP from HTTP: {}", addr->to_string());
    return {*std::move(addr)};
}
