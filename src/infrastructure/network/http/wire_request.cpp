//
// Shared request-building helpers for the net::http clients.
//
#include "infrastructure/network/http/wire_request.h"

#include <cctype>
#include <utility>

#include "infrastructure/network/uri.h"
#include "support/string_util.hpp"
#include "support/fmt.hpp"

namespace net::http {

namespace {

[[nodiscard]] bool is_token(const std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (const auto ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' || ch == '\'' ||
            ch == '*' || ch == '+' || ch == '-' || ch == '.' || ch == '^' || ch == '_' || ch == '`' || ch == '|' ||
            ch == '~') {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool is_field_value(const std::string_view value) noexcept {
    for (const auto ch: value) {
        const auto c = static_cast<unsigned char>(ch);
        if (c != '\t' && (c < 0x20 || c == 0x7f)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_managed_header(const std::string_view name) noexcept {
    return StringUtil::iequals(name, "host") || StringUtil::iequals(name, "content-length") ||
           StringUtil::iequals(name, "content-type") || StringUtil::iequals(name, "connection") ||
           StringUtil::iequals(name, "transfer-encoding") || StringUtil::iequals(name, "trailer") ||
           StringUtil::iequals(name, "upgrade");
}

} // namespace

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

std::string make_target(const Uri &uri) {
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

std::expected<void, Error> validate_request(const Request &req) {
    for (const auto &[name, value]: req.headers) {
        if (!is_token(name) || !is_field_value(value)) {
            return std::unexpected(Error{ErrorCode::INVALID_REQUEST, "invalid HTTP request header"});
        }
        if (StringUtil::iequals(name, "upgrade")) {
            return std::unexpected(Error{ErrorCode::UNSUPPORTED_PROTOCOL, "HTTP protocol upgrade is not supported"});
        }
        if (StringUtil::iequals(name, "transfer-encoding") || StringUtil::iequals(name, "trailer")) {
            return std::unexpected(Error{ErrorCode::INVALID_REQUEST, "request transfer coding and trailers are not supported"});
        }
    }
    if (!is_field_value(req.content_type)) {
        return std::unexpected(Error{ErrorCode::INVALID_REQUEST, "invalid HTTP Content-Type"});
    }
    return {};
}

protocol::WireRequest build_wire_request(const Request &req, const std::string_view scheme,
                                         const std::string_view host, const std::uint16_t port, const Options &opts) {
    protocol::WireRequest wire{
        .method = req.method,
        .version = opts.version,
        .target = {},
        .headers = {},
        .body = req.body,
    };
    for (const auto &[name, value]: req.headers) {
        if (!is_managed_header(name)) {
            wire.headers.emplace(name, value);
        }
    }

    wire.headers.emplace("Host", make_host_header(scheme, host, port));
    if (opts.version == HttpVersion::V1_0) {
        wire.headers.emplace("Connection", opts.keep_alive ? "keep-alive" : "close");
    } else if (!opts.keep_alive) {
        wire.headers.emplace("Connection", "close");
    }
    if (!opts.user_agent.empty()) {
        wire.headers.emplace("User-Agent", opts.user_agent);
    }
    if (req.body.has_value()) {
        wire.headers.emplace("Content-Length", std::to_string(req.body->size()));
        if (!req.content_type.empty()) {
            wire.headers.emplace("Content-Type", req.content_type);
        }
    }
    return wire;
}

Error map_connect_error(const Transport::IoError err) {
    using enum Transport::IoError;
    switch (err) {
        case CANCELLED:
            return {ErrorCode::CANCELLED, "connect/handshake: cancelled"};
        case TIMEOUT:
            return {ErrorCode::TIMEOUT, "connect/handshake: timed out"};
        case CONNECTION_FAILED:
            return {ErrorCode::CONNECT_FAILED, "connect/handshake failed"};
    }
    return {ErrorCode::CONNECT_FAILED, "connect/handshake failed"};
}

} // namespace net::http
