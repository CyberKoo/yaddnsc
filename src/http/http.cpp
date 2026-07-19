//
// Created by Kotarou on 2026/7/18.
//
#include "http.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include <expected>

#include "http/body_parser.h"
#include "http/header_parser.h"
#include "http/request.h"
#include "network/transport/stream.h"

namespace {

// ── Internal limits ──
constexpr size_t MAX_HEADER_SIZE = 8192;  ///< Per RFC 7230 §3.2, servers should limit to 8 KB.
constexpr size_t MAX_BODY_SIZE = 65536;   ///< Sensible default; callers that need more should
                                          ///< add their own Content-Length check before exchange.

/// Map a transport IoError to an HTTP Error.
[[nodiscard]] Http::Error map_io_error(Transport::IoError e) noexcept {
    switch (e) {
        case Transport::IoError::TIMEOUT:
            return Http::Error::TIMEOUT;
        case Transport::IoError::CANCELLED:
            return Http::Error::CANCELLED;
        case Transport::IoError::CONNECTION_FAILED:
            return Http::Error::CONNECTION_FAILED;
    }
    return Http::Error::CONNECTION_FAILED;
}

/// Read a complete HTTP/1.1 response from a transport stream.
[[nodiscard]] std::expected<Http::Response, Http::Error> read_response(Transport::Stream& stream,
                                                                       const Utils::CancellationToken& cancel_token) {
    constexpr size_t INITIAL_BUF_SIZE = 4096;
    std::vector<char> buf(std::min(INITIAL_BUF_SIZE, MAX_HEADER_SIZE));
    size_t total_read = 0;

    for (;;) {
        // Try to parse the accumulated data (no Content-Type filtering).
        auto result = Http::parse_response(std::string_view(buf.data(), total_read), "", MAX_BODY_SIZE);
        if (result) {
            const auto& headers = *result;

            // ── Phase 2: read body ──
            auto body =
                Http::read_body(stream, headers, std::span(buf.data(), total_read), MAX_BODY_SIZE, cancel_token);
            if (!body) {
                return std::unexpected(body.error());
            }

            return Http::Response{
                .status_code = headers.status_code,
                .body = std::move(*body),
            };
        }

        const auto err = result.error();

        // Non-recoverable parse error.
        if (err != Http::Error::INCOMPLETE) {
            return std::unexpected(err);
        }

        // Need more data — check size limit.
        if (total_read >= MAX_HEADER_SIZE) {
            return std::unexpected(Http::Error::HEADERS_TOO_LARGE);
        }

        // Grow buffer if full.
        if (total_read == buf.size()) {
            buf.resize(std::min(buf.size() * 2, MAX_HEADER_SIZE));
        }

        // Read more from transport.
        auto* read_ptr = reinterpret_cast<std::uint8_t*>(buf.data() + total_read);
        const auto read_capacity = buf.size() - total_read;

        auto read_result = stream.read_some(std::span<std::uint8_t>(read_ptr, read_capacity), cancel_token);

        if (!read_result) {
            return std::unexpected(map_io_error(read_result.error()));
        }

        if (*read_result == 0) {
            // Unexpected EOF — the transport closed before the response
            // headers were fully received.
            return std::unexpected(Http::Error::CONNECTION_FAILED);
        }

        total_read += *read_result;
    }
}
}  // anonymous namespace

namespace Http {

std::expected<Response, Error> exchange(Transport::Stream& stream,
                                        std::string_view path,
                                        const HttpRequest& req,
                                        std::string_view host_header,
                                        std::string_view user_agent,
                                        const Utils::CancellationToken& cancel_token) {
    // 1. Build wire-format request.
    auto wire = build_request(req, path, host_header, user_agent);

    // 2. Send.
    auto send = stream.send_all(wire, cancel_token);
    if (!send) {
        return std::unexpected(map_io_error(send.error()));
    }

    // 3. Read + parse response.
    return read_response(stream, cancel_token);
}

}  // namespace Http
