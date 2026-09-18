//
// Created by Kotarou on 2026/7/1.
//

#include "http.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <expected>
#include <spdlog/spdlog.h>
#include <yaddnsc/util/format.hpp>
#include <yaddnsc/util/string_util.hpp>

#include "domain/network/inet_address.h"
#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/persistent_client.h"
#include "infrastructure/network/http/types.h"
#include "infrastructure/network/transport/options.h"
#include "support/fmt.hpp"
#include "support/string_util.hpp"

#include "version.h"

namespace {
/// Build the client options for an HTTP IP source.
[[nodiscard]] net::http::Options make_client_options(const AddressFamily address_family,
                                                     const std::string& bind_interface) {
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
}  // namespace

// ===========================================================================
// HttpIpSource — fetch public IP from an external HTTP service.
// ===========================================================================

HttpIpSource::~HttpIpSource() = default;

HttpIpSource::HttpIpSource(std::string url, const AddressFamily address_family, std::string bind_interface)
    : url_(std::move(url)), address_family_(address_family), bind_interface_(std::move(bind_interface)),
      client_(std::make_unique<net::http::PersistentClient>(url_, make_client_options(address_family_, bind_interface_))) {}

// ---------------------------------------------------------------------------
// HttpIpSource::resolve — send GET request and parse the response body as an IP.
// ---------------------------------------------------------------------------

std::vector<InetAddress> HttpIpSource::resolve(const Utils::CancellationToken& token) const {
    net::http::Request req;
    req.method = net::http::Method::GET;

    auto resp = client_->exchange(url_, req, token);
    if (!resp) {
        throw std::runtime_error(
            fmt::format(R"(HTTP IP source "{}" did not return a valid response: {})", url_, resp.error().message));
    }

    auto addr = InetAddress::parse(StringUtil::trim(resp->text()));
    if (!addr) {
        throw std::runtime_error(fmt::format(R"(HTTP IP source "{}" did not return a valid message)", url_));
    }
    SPDLOG_DEBUG("Resolved IP from HTTP: {}", addr->to_string());
    return {*std::move(addr)};
}
