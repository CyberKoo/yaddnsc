//
// Redirect policy for the net::http client domain.
//
#include "http_client/redirect.h"

#include <algorithm>
#include <exception>
#include <string_view>
#include <utility>
#include <vector>

#include "fmt.hpp"
#include "string_util.hpp"

namespace net::http {

namespace {

/// A Location value resolved against the current request URI.
struct ResolvedLocation {
    std::string scheme;
    std::string host;
    std::uint16_t port;
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

[[nodiscard]] std::uint16_t default_port(const std::string_view scheme) noexcept {
    return scheme == "https" ? 443 : 80;
}

/// Format a Host header value per RFC 7230 §5.4 (bracket IPv6, omit
/// default ports).
[[nodiscard]] std::string make_host_header(const std::string_view host,
                                           const std::uint16_t port,
                                           const std::string_view scheme) {
    const bool is_ipv6 = host.find(':') != std::string_view::npos;
    auto bracketed = is_ipv6 ? fmt::format("[{}]", host) : std::string(host);
    if (port == default_port(scheme)) {
        return bracketed;
    }
    return fmt::format("{}:{}", bracketed, port);
}

[[nodiscard]] std::string make_target(const Uri& uri) {
    auto target = std::string(uri.get_path());
    if (target.empty()) {
        target = "/";
    }
    if (const auto query = uri.get_query_string(); !query.empty()) {
        target += '?';
        target += query;
    }
    return target;
}

/// RFC 3986 §5.2.4 dot-segment removal for an absolute request path.
[[nodiscard]] std::string remove_dot_segments(const std::string_view path) {
    std::vector<std::string_view> segments;
    for (size_t begin = 0; begin <= path.size();) {
        const auto end = path.find('/', begin);
        const auto segment = path.substr(begin, end == std::string_view::npos ? end : end - begin);
        if (segment == "..") {
            if (!segments.empty()) {
                segments.pop_back();
            }
        } else if (!segment.empty() && segment != ".") {
            segments.push_back(segment);
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }

    std::string result{"/"};
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            result += '/';
        }
        result += segments[i];
    }
    if (path.ends_with('/') && !result.ends_with('/')) {
        result += '/';
    }
    return result;
}

[[nodiscard]] std::string path_and_query(std::string_view reference) {
    const auto fragment = reference.find('#');
    reference = reference.substr(0, fragment);
    const auto query = reference.find('?');
    const auto path = reference.substr(0, query);
    auto target = remove_dot_segments(path.empty() ? "/" : path);
    if (query != std::string_view::npos) {
        target += reference.substr(query);
    }
    return target;
}

/// Resolve a Location header value against the current URI (RFC 3986 §5.2).
/// Supports absolute, scheme-relative, root-relative, path-relative, query-only,
/// and fragment-only references. Fragments are never sent in a request target.
[[nodiscard]] std::optional<ResolvedLocation> resolve_location(std::string_view location, const Uri& current_uri) {
    location = StringUtil::trim(location);
    if (location.empty()) {
        return std::nullopt;
    }

    std::string scheme{current_uri.get_schema()};
    std::string host{current_uri.get_host()};
    int raw_port = current_uri.get_port();
    std::string target;

    const auto scheme_separator = location.find(':');
    const bool absolute = scheme_separator != std::string_view::npos &&
                          location.substr(0, scheme_separator).find_first_of("/?#") == std::string_view::npos;
    if (absolute || location.starts_with("//")) {
        try {
            const auto uri = Uri::parse(location.starts_with("//") ? fmt::format("{}:{}", scheme, location)
                                                                   : std::string(location));
            scheme = std::string(uri.get_schema());
            host = std::string(uri.get_host());
            raw_port = uri.get_port();
            // Uri strips fragments; normalize the parsed path before sending it.
            target = path_and_query(make_target(uri));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    } else if (location.starts_with('/')) {
        target = path_and_query(location);
    } else if (location.starts_with('?')) {
        target = remove_dot_segments(current_uri.get_path()) +
                 std::string(location.substr(0, location.find('#')));
    } else if (location.starts_with('#')) {
        target = remove_dot_segments(current_uri.get_path());
        if (const auto query = current_uri.get_query_string(); !query.empty()) {
            target += '?';
            target += query;
        }
    } else {
        auto base = std::string(current_uri.get_path());
        base.resize(base.rfind('/') + 1);
        target = path_and_query(base + std::string(location));
    }

    if ((scheme != "http" && scheme != "https") || host.empty()) {
        return std::nullopt;
    }

    const auto port = static_cast<std::uint16_t>(raw_port > 0 ? raw_port : default_port(scheme));
    return ResolvedLocation{.scheme = scheme, .host = host, .port = port, .target = std::move(target),
                            .host_header = make_host_header(host, port, scheme)};
}

/// Drop hop-specific / body headers before rebuilding the request.
void strip_headers(protocol::WireRequest& req) {
    for (const auto* name : {"Content-Length", "Content-Type", "Host"}) {
        for (auto it = req.headers.begin(); it != req.headers.end();) {
            if (StringUtil::iequals(it->first, name)) {
                it = req.headers.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void strip_auth_headers(protocol::WireRequest& req) {
    for (const auto* name : {"Authorization", "Cookie", "Proxy-Authorization"}) {
        for (auto it = req.headers.begin(); it != req.headers.end();) {
            if (StringUtil::iequals(it->first, name)) {
                it = req.headers.erase(it);
            } else {
                ++it;
            }
        }
    }
}

}  // namespace

RedirectEval evaluate_redirect(const int status,
                               const std::multimap<std::string, std::string>& headers,
                               const int redirect_count,
                               const Options& opts,
                               const protocol::WireRequest& current,
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
    if (location.empty()) {
        return {};
    }

    if (!opts.follow_redirects) {
        return {};
    }
    if (redirect_count >= opts.max_redirects) {
        return {.plan = std::nullopt, .limit_reached = true};
    }

    const auto resolved = resolve_location(location, current_uri);
    if (!resolved) {
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

    // Preserve the original Content-Type (if any) before stripping headers.
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

}  // namespace net::http
