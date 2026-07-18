//
// Created by Kotarou on 2026/7/10.
//

#ifndef YADDNSC_HTTP_HEADER_PARSER_H
#define YADDNSC_HTTP_HEADER_PARSER_H

#include <cstddef>
#include <expected>
#include <string_view>

#include "http/types.h"

/// HTTP response header parsing using picohttpparser.
namespace Http {

/// Parse an HTTP response header block using picohttpparser.
///
/// @param buf                    Buffer containing at least a partial HTTP response.
/// @param expected_content_type  Expected Content-Type (e.g. "application/dns-message").
/// @param max_body_size          Maximum allowed body size.  Content-Length values
///                               exceeding this trigger BODY_TOO_LARGE.
///
/// @return  ResponseHeaders on success, or an Error describing the failure.
[[nodiscard]] std::expected<ResponseHeaders, Error> parse_response(
    std::string_view buf,
    std::string_view expected_content_type,
    size_t max_body_size);

}  // namespace Http

#endif  // YADDNSC_HTTP_HEADER_PARSER_H
