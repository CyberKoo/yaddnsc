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
#include <string_view>

#include "infrastructure/network/http/types.h"

namespace net::http::protocol {

/// Fully-resolved HTTP/1.1 request, ready for serialization.
struct WireRequest {
    Method method;
    HttpVersion version{HttpVersion::V1_1};
    std::string target{};                               ///< path + query, e.g. "/dns-query?x=1"
    std::multimap<std::string, std::string> headers{};  ///< includes Host / UA / CL / CT
    std::optional<std::string> body{};
};

/// HTTP method name as it appears on the wire.
[[nodiscard]] std::string_view method_name(Method m) noexcept;

/// Serialize to HTTP/1.0 or HTTP/1.1 wire format.
[[nodiscard]] std::string serialize(const WireRequest& req);

}  // namespace net::http::protocol

#endif  // YADDNSC_HTTP_CLIENT_PROTOCOL_WIRE_H
