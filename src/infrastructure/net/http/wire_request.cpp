//
// http — request building for the client.
//

#include "wire_request.h"

#include <map>
#include <optional>
#include <string>
#include <utility>

#include "infrastructure/net/http/protocol/field_chars.hpp"
#include "infrastructure/net/http/uri.h"
#include "support/fmt.hpp"
#include "support/string_util.hpp"

namespace http {
namespace {

/// The client owns these header fields: a user-supplied copy is discarded.
[[nodiscard]] bool is_managed_header(const std::string_view name) noexcept {
    return StringUtil::iequals(name, "host") || StringUtil::iequals(name, "content-length") ||
           StringUtil::iequals(name, "content-type") || StringUtil::iequals(name, "connection") ||
           StringUtil::iequals(name, "user-agent") || StringUtil::iequals(name, "transfer-encoding") ||
           StringUtil::iequals(name, "trailer") || StringUtil::iequals(name, "upgrade");
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
    if (const auto query = uri.get_query_string(); !query.empty()) {
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
        wire.headers.emplace("Connection", options.keep_alive ? "keep-alive" : "close");
    } else if (!options.keep_alive) {
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

Request to_public_request(const protocol::WireRequest& wire) {
    Request request{.method = wire.method};
    request.body = wire.body;
    for (const auto& [name, value] : wire.headers) {
        if (!is_managed_header(name)) {
            request.headers.emplace(name, value);
            continue;
        }
        if (StringUtil::iequals(name, "content-type")) {
            request.content_type = value;
        }
    }
    return request;
}

Error map_io_error(const net::IoError error, const std::string_view stage) {
    switch (error) {
        case net::IoError::CANCELLED:
            return {ErrorCode::CANCELLED, fmt::format("{}: cancelled", stage)};
        case net::IoError::CONNECTION_FAILED:
            return {ErrorCode::CONNECTION_LOST, fmt::format("{}: connection lost", stage)};
    }
    return {ErrorCode::CONNECTION_LOST, fmt::format("{}: connection lost", stage)};
}

Error map_connect_error(const net::IoError error) {
    if (error == net::IoError::CANCELLED) {
        return {ErrorCode::CANCELLED, "connect: cancelled"};
    }
    // TCP refusal and TLS handshake failure are not distinguished (the stream
    // reports one CONNECTION_FAILED for both), matching the legacy mapping.
    return {ErrorCode::CONNECT_FAILED, "connect failed"};
}

}  // namespace http
