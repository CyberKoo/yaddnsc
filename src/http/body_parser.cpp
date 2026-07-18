//
// Created by Kotarou on 2026/7/18.
//
#include "body_parser.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <expected>
#include <picohttpparser.h>

namespace Http {

std::expected<std::vector<std::uint8_t>, Error> read_body(Transport::Stream& stream,
                                                          const ResponseHeaders& headers,
                                                          std::span<const char> header_buf,
                                                          size_t max_body_size,
                                                          const Utils::CancellationToken& cancel_token) {
    const size_t body_buffered = header_buf.size() > headers.header_end ? header_buf.size() - headers.header_end : 0;

    // ── Fixed-length body (Content-Length) ──
    if (headers.has_content_length) {
        std::vector<std::uint8_t> body;
        body.reserve(headers.content_length);

        // Copy any already-buffered body data.
        if (body_buffered > 0) {
            auto src = header_buf.subspan(headers.header_end, body_buffered);
            const auto copy_size = (std::min) (body_buffered, headers.content_length);
            body.insert(body.end(), reinterpret_cast<const std::uint8_t*>(src.data()),
                        reinterpret_cast<const std::uint8_t*>(src.data() + copy_size));
        }

        // Read remaining from transport stream.
        if (body_buffered < headers.content_length) {
            body.resize(headers.content_length);
            auto* dst = body.data() + body_buffered;
            const auto remaining = headers.content_length - body_buffered;
            auto st = stream.read_exact(std::span(dst, remaining), cancel_token);
            if (!st) {
                switch (st.error()) {
                    case Transport::IoError::CANCELLED:
                        return std::unexpected(Error::CANCELLED);
                    case Transport::IoError::TIMEOUT:
                        return std::unexpected(Error::TIMEOUT);
                    default:
                        return std::unexpected(Error::CONNECTION_FAILED);
                }
            }
        }

        return body;
    }

    // ── Chunked transfer encoding ──
    if (!headers.is_chunked) {
        // No Content-Length and not chunked — there is no body.
        return std::vector<std::uint8_t>{};
    }

    // Collect any body data already received in the header buffer.
    std::vector<char> raw(body_buffered);
    if (body_buffered > 0) {
        auto src = header_buf.subspan(headers.header_end, body_buffered);
        std::copy(src.begin(), src.end(), raw.begin());
    }

    std::vector<std::uint8_t> body;
    body.reserve(max_body_size);

    phr_chunked_decoder decoder{};
    decoder.consume_trailer = 1;  // Automatically consume trailing headers.

    auto data_len = raw.size();

    for (;;) {
        auto bufsz = data_len;
        auto ret = phr_decode_chunked(&decoder, raw.data(), &bufsz);

        if (ret == -1) {
            return std::unexpected(Error::CHUNK_PARSE_FAILED);
        }

        // Collect decoded output produced in this call.
        if (bufsz > 0) {
            if (body.size() + bufsz > max_body_size) {
                return std::unexpected(Error::BODY_TOO_LARGE);
            }
            body.insert(body.end(), reinterpret_cast<const std::uint8_t*>(raw.data()),
                        reinterpret_cast<const std::uint8_t*>(raw.data() + bufsz));
        }

        if (ret >= 0) {
            // All chunks have been decoded.
            return body;
        }

        // ret == -2: incomplete — need more data from the stream.
        // All input data was consumed by the decoder (src == data_len),
        // so there is no unconsumed data to preserve.
        data_len = 0;

        // Read the next chunk of raw data from the transport.
        std::array<char, 4096> read_buf{};
        auto read_result = stream.read_some(
            std::span(reinterpret_cast<std::uint8_t*>(read_buf.data()), read_buf.size()), cancel_token);
        if (!read_result) {
            switch (read_result.error()) {
                case Transport::IoError::TIMEOUT:
                    return std::unexpected(Error::TIMEOUT);
                case Transport::IoError::CANCELLED:
                    return std::unexpected(Error::CANCELLED);
                default:
                    return std::unexpected(Error::CONNECTION_FAILED);
            }
        }

        data_len = *read_result;
        if (data_len == 0) {
            // Unexpected EOF during chunked body — a complete chunked
            // message always ends with "0\r\n\r\n".
            return std::unexpected(Error::CONNECTION_FAILED);
        }
        raw.assign(read_buf.data(), read_buf.data() + data_len);
    }
}

}  // namespace Http
