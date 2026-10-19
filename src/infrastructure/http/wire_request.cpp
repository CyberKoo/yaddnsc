//
// http — request building for the client.
//

#include "wire_request.h"

#include <algorithm>
#include <charconv>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <system_error>

#include "infrastructure/http/protocol/field_chars.hpp"
#include "infrastructure/uri/uri.h"
#include "support/fmt.hpp"
#include "support/string_util.hpp"  // IWYU pragma: keep — the StringUtil namespace alias lives here
#include "yaddnsc/util/format.hpp"  // IWYU pragma: keep — IWYU attributes coroutine lowering here; clangd does not
#include "yaddnsc/util/string_util.hpp"

namespace http {
namespace {

/// The client owns these header fields: a user-supplied copy is discarded.
[[nodiscard]] bool is_managed_header(const std::string_view name) noexcept {
    return StringUtil::iequals(name, "host") || StringUtil::iequals(name, "content-length") ||
           StringUtil::iequals(name, "content-type") || StringUtil::iequals(name, "connection") ||
           StringUtil::iequals(name, "user-agent") || StringUtil::iequals(name, "transfer-encoding") ||
           StringUtil::iequals(name, "trailer") || StringUtil::iequals(name, "upgrade");
}

/// RFC 9112 §3.2: the target is origin-form ("/..."), or "*" for a server-wide
/// request. Anything else would be a different request form than this client
/// ever sends.
[[nodiscard]] bool is_request_target(const std::string_view target) noexcept {
    if (target == "*") {
        return true;
    }
    if (target.empty() || target.front() != '/') {
        return false;
    }
    return std::ranges::none_of(target, [](const char character) {
        const auto value = static_cast<unsigned char>(character);
        return value <= 0x20 || value == 0x7f;
    });
}

}  // namespace

std::uint16_t default_port(const std::string_view scheme) noexcept {
    return scheme == "https" ? 443 : 80;
}

std::string make_host_header(const std::string_view scheme, const std::string_view host, const std::uint16_t port) {
    const bool is_ipv6 = host.find(':') != std::string_view::npos;
    auto bracketed = is_ipv6 ? fmt::format("[{}]", host) : std::string(host);
    if (port == default_port(scheme)) {
        return bracketed;
    }
    return fmt::format("{}:{}", bracketed, port);
}

std::string make_target(const Uri& uri) {
    auto target = std::string(uri.get_path());
    if (target.empty()) {
        target = "/";
    }
    if (const auto query = uri.get_query_string(); uri.has_query()) {
        target += '?';
        target += query;
    }
    return target;
}

std::expected<void, Error> validate_request(const Request& request) {
    for (const auto& [name, value] : request.headers) {
        if (!protocol::is_token(name) || !protocol::is_field_value(value)) {
            return std::unexpected(Error{ErrorCode::INVALID_REQUEST, "invalid HTTP request header"});
        }
        if (StringUtil::iequals(name, "upgrade")) {
            return std::unexpected(Error{ErrorCode::UNSUPPORTED_PROTOCOL, "HTTP protocol upgrade is not supported"});
        }
        if (StringUtil::iequals(name, "transfer-encoding") || StringUtil::iequals(name, "trailer")) {
            return std::unexpected(
                Error{ErrorCode::INVALID_REQUEST, "request transfer coding and trailers are not supported"});
        }
    }
    if (!protocol::is_field_value(request.content_type)) {
        return std::unexpected(Error{ErrorCode::INVALID_REQUEST, "invalid HTTP Content-Type"});
    }
    return {};
}

std::expected<void, Error> validate_wire_request(const protocol::WireRequest& wire) {
    if (!is_request_target(wire.target)) {
        return std::unexpected(Error{ErrorCode::INVALID_REQUEST, "invalid HTTP request target"});
    }

    std::size_t hosts = 0;
    std::size_t connections = 0;
    std::optional<std::size_t> content_length;
    for (const auto& [name, value] : wire.headers) {
        if (!protocol::is_token(name) || !protocol::is_field_value(value) ||
            (StringUtil::iequals(name, "connection") && !protocol::valid_token_list(value))) {
            return std::unexpected(Error{ErrorCode::INVALID_REQUEST, "invalid HTTP request header"});
        }
        if (StringUtil::iequals(name, "upgrade") ||
            (StringUtil::iequals(name, "connection") && protocol::has_token(value, "upgrade"))) {
            return std::unexpected(Error{ErrorCode::UNSUPPORTED_PROTOCOL, "HTTP protocol upgrade is not supported"});
        }
        if (StringUtil::iequals(name, "transfer-encoding") || StringUtil::iequals(name, "trailer")) {
            return std::unexpected(
                Error{ErrorCode::INVALID_REQUEST, "request transfer coding and trailers are not supported"});
        }
        if (StringUtil::iequals(name, "host")) {
            ++hosts;
        }
        if (StringUtil::iequals(name, "connection")) {
            ++connections;
        }
        if (StringUtil::iequals(name, "content-length")) {
            if (content_length) {
                return std::unexpected(
                    Error{ErrorCode::INVALID_REQUEST, "conflicting HTTP framing or routing headers"});
            }
            std::size_t parsed{};
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size()) {
                return std::unexpected(Error{ErrorCode::INVALID_REQUEST, "invalid HTTP Content-Length"});
            }
            content_length = parsed;
        }
    }
    if (hosts > 1 || connections > 1 || (content_length && *content_length != (wire.body ? wire.body->size() : 0))) {
        return std::unexpected(Error{ErrorCode::INVALID_REQUEST, "conflicting HTTP framing or routing headers"});
    }
    return {};
}

bool closes_connection(const Options& options) noexcept {
    return !options.keep_alive;
}

protocol::WireRequest build_wire_request(const Request& request, const std::string_view scheme,
                                         const std::string_view host, const std::uint16_t port,
                                         const Options& options) {
    protocol::WireRequest wire{
        .method = request.method,
        .version = options.version,
        .target = {},
        .headers = {},
        .body = request.body,
    };
    for (const auto& [name, value] : request.headers) {
        if (!is_managed_header(name)) {
            wire.headers.emplace(name, value);
        }
    }

    wire.headers.emplace("Host", make_host_header(scheme, host, port));
    if (options.version == HttpVersion::V1_0) {
        wire.headers.emplace("Connection", closes_connection(options) ? "close" : "keep-alive");
    } else if (closes_connection(options)) {
        wire.headers.emplace("Connection", "close");
    }
    if (!options.user_agent.empty()) {
        wire.headers.emplace("User-Agent", options.user_agent);
    }
    if (request.body.has_value()) {
        wire.headers.emplace("Content-Length", std::to_string(request.body->size()));
        if (!request.content_type.empty()) {
            wire.headers.emplace("Content-Type", request.content_type);
        }
    }
    return wire;
}

std::string make_absolute_url(const std::string_view scheme, const std::string_view host, const std::uint16_t port,
                              const std::string_view target) {
    const bool ipv6 = host.find(':') != std::string_view::npos;
    return fmt::format("{}://{}{}{}:{}{}", scheme, ipv6 ? "[" : "", host, ipv6 ? "]" : "", port, target);
}

}  // namespace http
