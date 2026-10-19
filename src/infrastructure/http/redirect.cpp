#include "redirect.h"

#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

#include "infrastructure/http/wire_request.h"
#include "infrastructure/uri/uri.h"
#include "yaddnsc/util/string_util.hpp"

namespace http {
namespace {

[[nodiscard]] bool is_redirect_status(int status) noexcept {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

void strip_auth_headers(Request& request) {
    std::erase_if(request.headers, [](const auto& field) {
        return yaddnsc::util::iequals(field.first, "Authorization") || yaddnsc::util::iequals(field.first, "Cookie") ||
               yaddnsc::util::iequals(field.first, "Proxy-Authorization");
    });
}

}  // namespace

RedirectEval evaluate_redirect(const int status, const std::multimap<std::string, std::string>& headers,
                               const int redirect_count, const Options& options, const Request& current,
                               const Uri& current_uri) {
    if (!is_redirect_status(status)) {
        return {};
    }
    std::string_view location;
    for (const auto& [name, value] : headers) {
        if (yaddnsc::util::iequals(name, "location")) {
            location = value;
            break;
        }
    }
    if (location.empty() || !options.follow_redirects) {
        return {};
    }
    if (redirect_count >= options.max_redirects) {
        return {.plan = std::nullopt, .limit_reached = true};
    }
    location = yaddnsc::util::trim(location);
    if (location.empty()) {
        return {};
    }
    const auto resolved = current_uri.resolve(location);
    if (!resolved || (resolved->get_schema() != "http" && resolved->get_schema() != "https") ||
        (current_uri.get_schema() == "https" && resolved->get_schema() == "http")) {
        return {};
    }
    const auto port = static_cast<std::uint16_t>(resolved->get_port() > 0 ? resolved->get_port()
                                                                          : default_port(resolved->get_schema()));
    const auto current_port = static_cast<std::uint16_t>(
        current_uri.get_port() > 0 ? current_uri.get_port() : default_port(current_uri.get_schema()));
    RedirectPlan plan{
        .next = current,
        .url = make_absolute_url(resolved->get_schema(), resolved->get_host(), port, make_target(*resolved)),
        .cross_origin = resolved->get_schema() != current_uri.get_schema() ||
                        resolved->get_host() != current_uri.get_host() || port != current_port,
    };
    if (status != 307 && status != 308) {
        plan.next.method = Method::GET;
        plan.next.body = std::nullopt;
        plan.next.content_type.clear();
    }
    if (plan.cross_origin) {
        strip_auth_headers(plan.next);
    }
    return {.plan = std::move(plan)};
}

}  // namespace http
