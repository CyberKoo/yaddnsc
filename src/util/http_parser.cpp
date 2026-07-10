//
// Created by Kotarou on 2026/7/10.
//
#include "http_parser.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <expected>
#include <string>

#include <picohttpparser.h>

#include "string_util.hpp"

namespace Utils::Http {
    std::expected<ResponseInfo, HttpError> parse_response(std::string_view buf, std::string_view expected_content_type,
                                                          size_t max_body_size) {
        int minor_version;
        int status_code;
        const char *msg;
        size_t msg_len;
        phr_header headers[64];
        size_t num_headers = 64;

        const int pret = phr_parse_response(buf.data(), buf.size(), &minor_version, &status_code, &msg, &msg_len,
                                            headers, &num_headers, 0);

        if (pret == -2) {
            return std::unexpected(HttpError::Incomplete);
        }
        if (pret == -1) {
            return std::unexpected(HttpError::ParseFailed);
        }

        // Headers parsed successfully.
        const auto header_end = static_cast<size_t>(pret);

        size_t content_length = 0;
        bool has_content_length = false;
        bool valid_content_type = false;
        bool is_chunked = false;

        for (size_t i = 0; i < num_headers; ++i) {
            const std::string_view hname(headers[i].name, headers[i].name_len);
            const std::string_view hvalue(headers[i].value, headers[i].value_len);

            if (StringUtil::iequals(hname, "content-length") && !has_content_length) {
                try {
                    content_length = std::stoul(std::string(hvalue));
                } catch (const std::exception &) {
                    return std::unexpected(HttpError::ParseFailed);
                }
                has_content_length = true;
            } else if (StringUtil::iequals(hname, "content-type") && !valid_content_type) {
                if (StringUtil::icontains(hvalue, expected_content_type)) {
                    valid_content_type = true;
                }
            } else if (StringUtil::iequals(hname, "transfer-encoding")) {
                if (StringUtil::icontains(hvalue, "chunked")) {
                    is_chunked = true;
                }
            }
        }

        if (!valid_content_type) {
            return std::unexpected(HttpError::ContentTypeMismatch);
        }

        if (has_content_length && content_length > max_body_size) {
            return std::unexpected(HttpError::BodyTooLarge);
        }

        return ResponseInfo{
            .status_code = status_code,
            .content_length = content_length,
            .header_end = header_end,
            .has_content_length = has_content_length,
            .is_chunked = is_chunked,
        };
    }
} // namespace Utils::Http
