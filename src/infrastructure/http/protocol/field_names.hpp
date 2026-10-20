#ifndef YADDNSC_INFRASTRUCTURE_HTTP_PROTOCOL_FIELD_NAMES_HPP
#define YADDNSC_INFRASTRUCTURE_HTTP_PROTOCOL_FIELD_NAMES_HPP

#include <string_view>

/// HTTP field names the client interprets (RFC 9110 §5.1 — field names are
/// case-insensitive).
namespace http::protocol::field {

inline constexpr std::string_view HOST = "host";
inline constexpr std::string_view CONTENT_LENGTH = "content-length";
inline constexpr std::string_view CONTENT_TYPE = "content-type";
inline constexpr std::string_view CONNECTION = "connection";
inline constexpr std::string_view KEEP_ALIVE = "keep-alive";
inline constexpr std::string_view USER_AGENT = "user-agent";
inline constexpr std::string_view TRANSFER_ENCODING = "transfer-encoding";
inline constexpr std::string_view TRAILER = "trailer";
inline constexpr std::string_view UPGRADE = "upgrade";

}  // namespace http::protocol::field

#endif  // YADDNSC_INFRASTRUCTURE_HTTP_PROTOCOL_FIELD_NAMES_HPP
