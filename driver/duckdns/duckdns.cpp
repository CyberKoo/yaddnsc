//
// Created by Kotarou on 2026/7/13.
//

#include "duckdns.h"

#include <glaze/glaze.hpp>

#include "fmt.hpp"
#include "config.hpp"
#include "driver/factory.h"
#include "interface/core_logger.h"

namespace {
    constexpr std::string_view API_URL = "https://www.duckdns.org/update";
}

DEFINE_DRIVER_FACTORY(DuckDnsDriver)

DriverRequestContext DuckDnsDriver::generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const {
    auto cfg = parse_config<DuckDnsParams>(config);

    // Use ipv6 param for AAAA records, ip param for A records
    auto ip_param = (ctx.rd_type == "AAAA") ? "ipv6" : "ip";

    auto url = fmt::format("{}?domains={}&token={}&{}={}",
                           API_URL, ctx.subdomain, cfg.token, ip_param, ctx.ip_addr);

    if (cfg.verbose.value_or(false)) {
        url += "&verbose=true";
    }

    DriverRequest request{};
    request.method = DriverHttpMethod::GET;

    return {std::move(url), std::move(request)};
}

bool DuckDnsDriver::check_response(const HttpResponse &response) const {
    CORE_LOG_TRACE("Got {} from server.", response.body);

    // DuckDNS returns:
    //   "OK"           — success (non-verbose)
    //   "OK\n..."      — success (verbose mode)
    //   "KO"           — failure
    if (response.body.starts_with("OK")) {
        if (response.body.size() > 2) {
            CORE_LOG_DEBUG("DNS record updated successfully: {}", response.body);
        }
        return true;
    }

    CORE_LOG_ERROR("DuckDNS API error: {}", response.body);
    return false;
}

DriverDetail DuckDnsDriver::get_detail() const noexcept {
    return {
        .name = "duckdns",
        .description = "Updates DNS records via the DuckDNS API",
        .author = "Kotarou",
        .version = "1.0.0"
    };
}
