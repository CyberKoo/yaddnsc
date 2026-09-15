//
// Shared request-building helpers for the net::http clients.
//
#include "http_client/wire_request.h"

#include <utility>

#include "uri.h"

#include "fmt.hpp"

namespace net::http {

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

protocol::WireRequest build_wire_request(const Request &req, const std::string_view scheme,
                                         const std::string_view host, const std::uint16_t port, const Options &opts) {
    protocol::WireRequest wire{
        .method = req.method,
        .version = opts.version,
        .target = {},
        .headers = req.headers,
        .body = req.body,
    };
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
