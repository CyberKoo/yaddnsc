//
// http — redirect policy.
//

#include "redirect.h"

#include <initializer_list>
#include <string_view>
#include <utility>

#include <expected>

#include "infrastructure/http/types.h"
#include "infrastructure/http/wire_request.h"
#include "infrastructure/uri/uri.h"
#include "support/string_util.hpp"  // IWYU pragma: keep — the StringUtil namespace alias lives here
#include "yaddnsc/util/string_util.hpp"

namespace http {
namespace {

/// A Location value resolved against the current request URI.
struct ResolvedLocation {
    std::string scheme;
    std::string host;
    std::uint16_t port{0};
    std::string target;  ///< path + query for the request line.
    std::string host_header;
};

[[nodiscard]] bool is_redirect_status(const int status) noexcept {
    switch (status) {
        case 301:
        case 302:
        case 303:
        case 307:
        case 308:
            return true;
        default:
            return false;
    }
}

/// Resolve the URI reference; HTTP-specific policy remains in this layer.
[[nodiscard]] std::optional<ResolvedLocation> resolve_location(std::string_view location, const Uri& current_uri) {
    location = StringUtil::trim(location);
    if (location.empty()) {
        return std::nullopt;
    }
    const auto uri = current_uri.resolve(location);
    if (!uri || (uri->get_schema() != "http" && uri->get_schema() != "https")) {
        return std::nullopt;
    }
    const std::string scheme{uri->get_schema()};
    const std::string host{uri->get_host()};
    const auto port = static_cast<std::uint16_t>(uri->get_port() > 0 ? uri->get_port() : default_port(scheme));
    return ResolvedLocation{.scheme = scheme,
                            .host = host,
                            .port = port,
                            .target = make_target(*uri),
                            .host_header = make_host_header(scheme, host, port)};
}

/// Drop hop-specific and body headers before rebuilding the request.
void strip_headers(protocol::WireRequest& request) {
    for (const auto* name : {"Content-Length", "Content-Type", "Host"}) {
        for (auto it = request.headers.begin(); it != request.headers.end();) {
            if (StringUtil::iequals(it->first, name)) {
                it = request.headers.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void strip_auth_headers(protocol::WireRequest& request) {
    for (const auto* name : {"Authorization", "Cookie", "Proxy-Authorization"}) {
        for (auto it = request.headers.begin(); it != request.headers.end();) {
            if (StringUtil::iequals(it->first, name)) {
                it = request.headers.erase(it);
            } else {
                ++it;
            }
        }
    }
}

}  // namespace

RedirectEval evaluate_redirect(const int status, const std::multimap<std::string, std::string>& headers,
                               const int redirect_count, const Options& options, const protocol::WireRequest& current,
                               const Uri& current_uri) {
    if (!is_redirect_status(status)) {
        return {};
    }

    std::string_view location;
    for (const auto& [name, value] : headers) {
        if (StringUtil::iequals(name, "location")) {
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

    const auto resolved = resolve_location(location, current_uri);
    if (!resolved) {
        return {};
    }

    // Never follow an https -> http downgrade: that would silently strip TLS
    // from a request the caller expected to be encrypted.
    if (current_uri.get_schema() == "https" && resolved->scheme == "http") {
        return {};
    }

    const auto current_port = static_cast<std::uint16_t>(
        current_uri.get_port() > 0 ? current_uri.get_port() : default_port(current_uri.get_schema()));

    RedirectPlan plan{
        .next = current,
        .scheme = resolved->scheme,
        .host = resolved->host,
        .port = resolved->port,
        .cross_origin = resolved->scheme != current_uri.get_schema() || resolved->host != current_uri.get_host() ||
                        resolved->port != current_port,
    };

    // 307/308 preserve the method and body; 301/302/303 rewrite to GET.
    const bool preserve_method = status == 307 || status == 308;
    if (!preserve_method) {
        plan.next.method = Method::GET;
        plan.next.body = std::nullopt;
    }
    plan.next.target = resolved->target;

    std::string content_type;
    for (const auto& [name, value] : plan.next.headers) {
        if (StringUtil::iequals(name, "content-type")) {
            content_type = value;
            break;
        }
    }

    strip_headers(plan.next);
    plan.next.headers.emplace("Host", resolved->host_header);
    if (plan.next.body.has_value()) {
        plan.next.headers.emplace("Content-Length", std::to_string(plan.next.body->size()));
        if (!content_type.empty()) {
            plan.next.headers.emplace("Content-Type", content_type);
        }
    }

    if (plan.cross_origin) {
        strip_auth_headers(plan.next);
    }

    return {.plan = std::move(plan)};
}

}  // namespace http
