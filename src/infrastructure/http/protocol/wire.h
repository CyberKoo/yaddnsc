//
// http — wire-level request representation and serialization.
//
// A WireRequest is fully resolved: the target already contains path + query and
// the headers already carry Host, Connection, User-Agent, Content-Length and
// Content-Type as applicable. serialize() produces the exact bytes sent.
//

#ifndef YADDNSC_INFRASTRUCTURE_HTTP_PROTOCOL_WIRE_H
#define YADDNSC_INFRASTRUCTURE_HTTP_PROTOCOL_WIRE_H

#include <optional>
#include <string>
#include <string_view>
#include <map>

#include "infrastructure/http/types.h"

namespace http::protocol {

/// Fully-resolved HTTP/1.x request, ready for serialization.
struct WireRequest {
    Method method{Method::GET};
    HttpVersion version{HttpVersion::V1_1};
    std::string target{};  ///< path + query, e.g. "/dns-query?x=1"
    std::multimap<std::string, std::string> headers{};
    std::optional<std::string> body{};
};

/// HTTP method name as it appears on the wire.
[[nodiscard]] std::string_view method_name(Method method) noexcept;

/// Serialize to HTTP/1.0 or HTTP/1.1 wire format. Allocates.
[[nodiscard]] std::string serialize(const WireRequest& request);

}  // namespace http::protocol

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_PROTOCOL_WIRE_H
