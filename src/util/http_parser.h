//
// Created by Kotarou on 2026/7/10.
//

#ifndef YADDNSC_UTIL_HTTP_PARSER_H
#define YADDNSC_UTIL_HTTP_PARSER_H

#include <cstddef>
#include <expected>
#include <string_view>

/// HTTP response header parsing using picohttpparser.
namespace Utils::Http {

/// Parsed HTTP response headers.
struct ResponseInfo {
    int status_code;        ///< HTTP status code (e.g. 200, 404).
    size_t content_length;  ///< Value of Content-Length header (valid only when has_content_length).
    size_t header_end;      ///< Offset of body start in the source buffer.
    bool has_content_length{false}; ///< Whether Content-Length was present.
    bool is_chunked{false};        ///< Whether Transfer-Encoding: chunked was present.
};

/// Errors that can occur during HTTP response header parsing.
enum class HttpError {
    Incomplete,          ///< More data is needed (picohttpparser returned -2).
    ParseFailed,         ///< Malformed HTTP response (picohttpparser returned -1).
    ContentTypeMismatch, ///< Content-Type does not contain @p expected_content_type.
    BodyTooLarge,        ///< Content-Length exceeds @p max_body_size.
};

/// Parse an HTTP response header block using picohttpparser.
///
/// @param buf                    Buffer containing at least a partial HTTP response.
/// @param expected_content_type  Expected Content-Type (e.g. "application/dns-message").
/// @param max_body_size          Maximum allowed body size.  Content-Length values
///                               exceeding this trigger BodyTooLarge.
///
/// @return  ResponseInfo on success, or an HttpError describing the failure.
[[nodiscard]] std::expected<ResponseInfo, HttpError> parse_response(
    std::string_view buf,
    std::string_view expected_content_type,
    size_t max_body_size);

}  // namespace Utils::Http

#endif  // YADDNSC_UTIL_HTTP_PARSER_H
