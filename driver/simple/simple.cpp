//
// Created by Kotarou on 2022/4/5.
//

#include "simple.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>
#include <string_view>

#include <glaze/glaze.hpp>

namespace fmt = yaddnsc::sdk::fmt;
using yaddnsc::sdk::Error;
using yaddnsc::sdk::HttpRequest;
using yaddnsc::sdk::HttpResponse;
using yaddnsc::sdk::Method;
using yaddnsc::sdk::Result;
using yaddnsc::sdk::Services;
using yaddnsc::sdk::UpdateContext;
using yaddnsc::sdk::UpdateRequest;

namespace {
    constexpr std::string_view DRIVER_NAME = "simple";

    // Copied from include/string_util.hpp (host-internal; plugins must not
    // include it) so the URL template substitution behaves identically.
    void replace_all(std::string &str, const std::string_view target, const std::string_view replacement) {
        if (target.empty()) return;

        // Equal-length: in-place overwrite, no allocation
        if (target.size() == replacement.size()) {
            if (replacement.data() >= str.data() && replacement.data() < str.data() + str.size()) {
                std::string repl(replacement);
                auto pos = str.find(target);
                while (pos != std::string::npos) {
                    std::copy_n(repl.data(), replacement.size(), str.data() + pos);
                    pos = str.find(target, pos + replacement.size());
                }
            } else {
                auto pos = str.find(target);
                while (pos != std::string::npos) {
                    std::copy_n(replacement.data(), replacement.size(), str.data() + pos);
                    pos = str.find(target, pos + replacement.size());
                }
            }
            return;
        }

        // Unequal-length: build new string, single allocation
        std::string result;
        size_t last = 0;
        auto pos = str.find(target);
        while (pos != std::string::npos) {
            result.append(str, last, pos - last);
            result.append(replacement);
            last = pos + target.size();
            pos = str.find(target, last);
        }
        result.append(str, last);
        str.swap(result);
    }

    // Copied from include/string_util.hpp.
    std::string_view ltrim(const std::string_view sv) noexcept {
        const auto it = std::ranges::find_if(sv, [](unsigned char ch) noexcept {
            return !std::isspace(ch);
        });
        return sv.substr(static_cast<size_t>(std::distance(sv.begin(), it)));
    }

    // Copied from include/string_util.hpp.
    std::string_view rtrim(const std::string_view sv) noexcept {
        const auto it = std::ranges::find_if(
                sv | std::views::reverse,
                [](unsigned char ch) noexcept {
                    return !std::isspace(ch);
                });
        return sv.substr(0, static_cast<size_t>(std::distance(sv.begin(), it.base())));
    }

    // Copied from include/string_util.hpp.
    std::string_view trim(const std::string_view sv) noexcept {
        return ltrim(rtrim(sv));
    }
} // namespace

YADDNSC_DEFINE_DRIVER(SimpleDriver, "simple", "Generic HTTP driver with URL template substitution", "Kotarou",
                      "2.0.0", YADDNSC_DRIVER_CAPABILITY_A | YADDNSC_DRIVER_CAPABILITY_AAAA)

Result SimpleDriver::update(UpdateContext &context) {
    const auto &params = context.request();

    auto request = generate_request(params);

    YADDNSC_SDK_LOG_DEBUG(context, "Domain {} ({}) received DNS record update request from driver {}, {}",
                          params.fqdn, params.record_type, DRIVER_NAME, yaddnsc::sdk::format_request(request));

    auto response = context.exchange(request);
    if (!response) {
        YADDNSC_SDK_LOG_WARN(context, "Domain {} ({}) update failed (HTTP error: {})", params.fqdn,
                             params.record_type, response.error().message);
        return std::unexpected(Error{response.error().status, response.error().message, 0});
    }

    if (!check_response(*response, context.services())) {
        YADDNSC_SDK_LOG_WARN(context, "Domain {} ({}) update rejected by upstream", params.fqdn, params.record_type);
        return std::unexpected(Error{YADDNSC_STATUS_UPSTREAM_REJECTED,
                                     fmt::format("Domain {} ({}) update rejected by upstream", params.fqdn,
                                                 params.record_type),
                                     0});
    }

    return {};
}

HttpRequest SimpleDriver::generate_request(const UpdateRequest &params) {
    auto full = parse_config<glz::generic>(params.driver_param_json);
    if (!full.is_object() || !full.contains("url") || !full["url"].is_string()) {
        throw yaddnsc::sdk::ConfigParseError(
                "Driver configuration parse error: Missing required parameter \"url\" in driver config");
    }

    auto &obj = full.get_object();
    auto url = obj["url"].get_string();

    // Substitute all keys into the URL template: config params first, then context
    const auto substitute = [&](std::string_view key, std::string_view val) {
        const auto target = fmt::format("{{{}}}", key);
        replace_all(url, target, val);
    };

    for (auto &[key, val]: obj) {
        if (key != "url" && val.is_string()) {
            substitute(key, val.get_string());
        }
    }

    substitute("ip_addr", params.ip_address);
    substitute("rd_type", params.record_type);
    substitute("domain", params.domain);
    substitute("subdomain", params.subdomain);
    substitute("fqdn", params.fqdn);

    HttpRequest request{};
    request.url = std::move(url);
    request.method = Method::Get;
    return request;
}

bool SimpleDriver::check_response(const HttpResponse &response, const Services &services) {
    YADDNSC_SDK_LOG_DEBUG(services, "Status: {}, Response: {}", response.status_code, trim(response.body));

    if (response.status_code >= 300) {
        YADDNSC_SDK_LOG_ERROR(services, "HTTP request failed with status code {}", response.status_code);
        return false;
    }

    return !response.body.empty();
}
