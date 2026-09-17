//
// Wire-level request representation and serialization for net::http.
//
// A WireRequest is fully resolved: the target already contains path+query
// and the headers already include Host, User-Agent, Content-Length and
// Content-Type as applicable. serialize() produces the exact bytes sent
// to the transport stream.
//

#ifndef YADDNSC_HTTP_CLIENT_PROTOCOL_WIRE_H
#define YADDNSC_HTTP_CLIENT_PROTOCOL_WIRE_H

#include <map>
#include <optional>
#include <string>

#include "http/types.h"

namespace net::http::protocol {

/// Fully-resolved HTTP/1.1 request, ready for serialization.
struct WireRequest {
    Method method;
    HttpVersion version{HttpVersion::V1_1};
    std::string target;                               ///< path + query, e.g. "/dns-query?x=1"
    std::multimap<std::string, std::string> headers;  ///< includes Host / UA / CL / CT
    std::optional<std::string> body;
};

/// HTTP method name as it appears on the wire.
[[nodiscard]] constexpr std::string_view method_name(const Method m) noexcept {
    using enum Method;
    switch (m) {
        case GET:
            return "GET";
        case POST:
            return "POST";
        case PUT:
            return "PUT";
        case DEL:
            return "DELETE";
        case PATCH:
            return "PATCH";
        case HEAD:
            return "HEAD";
        case OPTIONS:
            return "OPTIONS";
    }
    return "GET";
}

/// Serialize to HTTP/1.0 or HTTP/1.1 wire format.
[[nodiscard]] inline std::string serialize(const WireRequest& req) {
    std::string out;
    const size_t body_size = req.body.has_value() ? req.body->size() : 0;
    out.reserve(64 + req.target.size() + body_size);

    out += method_name(req.method);
    out += ' ';
    out += req.target.empty() ? "/" : req.target;
    out += req.version == HttpVersion::V1_0 ? " HTTP/1.0\r\n" : " HTTP/1.1\r\n";

    for (const auto& [name, value] : req.headers) {
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    out += "\r\n";
    if (body_size > 0) {
        out += *req.body;
    }
    return out;
}

}  // namespace net::http::protocol

#endif  // YADDNSC_HTTP_CLIENT_PROTOCOL_WIRE_H
