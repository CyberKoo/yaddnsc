//
// Created by Kotarou on 2026/7/18.
//

#ifndef YADDNSC_HTTP_REQUEST_H
#define YADDNSC_HTTP_REQUEST_H

#include <cstdint>
#include <string_view>
#include <vector>

#include "http_type.h"

namespace Http {

/// Build the wire-format bytes for an HTTP/1.1 request.
///
/// @param req            Request parameters (method, headers, body, content_type).
/// @param path           Request path (e.g. "/dns-query").
/// @param host_header    Value of the Host header (pre-formatted, e.g.
///                       "example.com" or "[::1]:443").
/// @param user_agent     User-Agent header value.
///
/// @return  The complete HTTP/1.1 request as a byte vector.
[[nodiscard]] std::vector<std::uint8_t> build_request(const HttpRequest& req,
                                                      std::string_view path,
                                                      std::string_view host_header,
                                                      std::string_view user_agent);

}  // namespace Http

#endif  // YADDNSC_HTTP_REQUEST_H
