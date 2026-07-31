//
// Created by Kotarou on 2026/7/10.
//
#include "header_parser.h"

#include <charconv>

#include <expected>
#include <picohttpparser.h>

#include "string_util.hpp"

namespace Http {
std::expected<ResponseHeaders, Error> parse_response(std::string_view buf,
                                                     std::string_view expected_content_type,
                                                     size_t max_body_size) {
    int status_code;
    // phr_parse_response requires non-null pointers for these even when
    // the caller does not need the values — it dereferences them immediately.
    [[maybe_unused]] int minor_version;
    [[maybe_unused]] const char* msg;
    [[maybe_unused]] size_t msg_len;
    phr_header headers[64];
    size_t num_headers = 64;

    const auto pret = phr_parse_response(buf.data(), buf.size(), &minor_version, &status_code, &msg, &msg_len, headers,
                                         &num_headers, 0);

    if (pret == -2) {
        return std::unexpected(Error::INCOMPLETE);
    }
    if (pret == -1) {
        return std::unexpected(Error::HEADER_PARSE_FAILED);
    }

    // Headers parsed successfully.
    const auto header_end = static_cast<size_t>(pret);

    size_t content_length = 0;
    bool has_content_length = false;
    bool has_transfer_encoding = false;
    bool valid_content_type = false;
    bool is_chunked = false;

    for (size_t i = 0; i < num_headers; ++i) {
        const auto hname = std::string_view(headers[i].name, headers[i].name_len);
        const auto hvalue = std::string_view(headers[i].value, headers[i].value_len);

        if (StringUtil::iequals(hname, "content-length")) {
            // RFC 7230 §3.3.2: Content-Length is a single decimal value.
            // Tolerate optional whitespace around the digits, but reject
            // anything else (e.g. "100evil" parses as 100 via from_chars
            // unless the end of input is verified).
            const auto trimmed = StringUtil::trim(hvalue);
            if (trimmed.empty()) {
                return std::unexpected(Error::HEADER_PARSE_FAILED);
            }
            size_t parsed = 0;
            auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
            if (ec != std::errc() || ptr != trimmed.data() + trimmed.size()) {
                return std::unexpected(Error::HEADER_PARSE_FAILED);
            }
            if (!has_content_length) {
                content_length = parsed;
                has_content_length = true;
            } else if (content_length != parsed) {
                // Multiple Content-Length headers with conflicting values
                // are an HTTP request/response smuggling vector.
                return std::unexpected(Error::HEADER_PARSE_FAILED);
            }
            // Identical repeated values are tolerated (RFC 7230 §3.3.2).
        } else if (StringUtil::iequals(hname, "content-type")) {
            if (StringUtil::icontains(hvalue, expected_content_type)) {
                valid_content_type = true;
            }
        } else if (StringUtil::iequals(hname, "transfer-encoding")) {
            has_transfer_encoding = true;
            if (StringUtil::icontains(hvalue, "chunked")) {
                is_chunked = true;
            }
        }
    }

    // RFC 7230 §3.3.3: Content-Length and Transfer-Encoding must not both
    // be present — ambiguous framing is a smuggling vector.
    if (has_content_length && has_transfer_encoding) {
        return std::unexpected(Error::HEADER_PARSE_FAILED);
    }

    // Transfer-Encoding without a final chunked coding is unsupported.
    if (has_transfer_encoding && !is_chunked) {
        return std::unexpected(Error::HEADER_PARSE_FAILED);
    }

    if (!valid_content_type && !expected_content_type.empty()) {
        return std::unexpected(Error::CONTENT_TYPE_MISMATCH);
    }

    if (has_content_length && content_length > max_body_size) {
        return std::unexpected(Error::BODY_TOO_LARGE);
    }

    return ResponseHeaders{
        .status_code = status_code,
        .content_length = content_length,
        .header_end = header_end,
        .has_content_length = has_content_length,
        .is_chunked = is_chunked,
    };
}
}  // namespace Http
