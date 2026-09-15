//
// Full HTTP/1.1 request-response exchange over a Transport::Stream.
//
#include "http_client/protocol/exchange.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <expected>
#include <picohttpparser.h>
#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "string_util.hpp"

namespace net::http::protocol {

namespace {

/// Maximum number of response headers parsed per message.
constexpr size_t MAX_HEADERS = 64;

/// Grow step for the response header buffer.
constexpr size_t READ_CHUNK = 4096;

/// Result of an incremental header parse attempt.
struct HeaderOutcome {
    bool ok = false;          ///< Headers fully parsed.
    bool incomplete = false;  ///< Need more bytes (-2).
    int status = 0;           ///< HTTP status code (valid when ok).
    int minor_version = 1;    ///< HTTP/1.x minor version.
    size_t header_end = 0;    ///< Offset of body start (valid when ok).
    size_t content_length = 0;
    bool has_content_length = false;
    bool is_chunked = false;
    bool connection_close = false;
    bool connection_keep_alive = false;
    std::multimap<std::string, std::string> headers;
    Error error{ErrorCode::RESPONSE_PARSE_FAILED, {}};
};

[[nodiscard]] std::string context(const WireRequest& req) {
    return fmt::format("{} {}", method_name(req.method), req.target);
}

/// Parse accumulated bytes as response headers (single pass).
[[nodiscard]] HeaderOutcome parse_headers(const std::string_view buf, const Limits& limits) {
    HeaderOutcome out;

    int status = 0;
    int minor_version = 0;
    const char* msg = nullptr;
    size_t msg_len = 0;
    std::array<phr_header, MAX_HEADERS> headers{};
    size_t num_headers = MAX_HEADERS;

    const auto pret = phr_parse_response(buf.data(), buf.size(), &minor_version, &status, &msg, &msg_len,
                                         headers.data(), &num_headers, 0);
    if (pret == -2) {
        out.incomplete = true;
        return out;
    }
    if (pret == -1) {
        out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "malformed response headers"};
        return out;
    }

    if (minor_version != 0 && minor_version != 1) {
        out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "unsupported HTTP version"};
        return out;
    }

    out.ok = true;
    out.status = status;
    out.minor_version = minor_version;
    out.header_end = static_cast<size_t>(pret);

    bool has_transfer_encoding = false;
    for (size_t i = 0; i < num_headers; ++i) {
        const auto name = std::string_view(headers[i].name, headers[i].name_len);
        const auto value = std::string_view(headers[i].value, headers[i].value_len);

        if (StringUtil::iequals(name, "content-length")) {
            const auto trimmed = StringUtil::trim(value);
            size_t parsed = 0;
            const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
            if (trimmed.empty() || ec != std::errc() || ptr != trimmed.data() + trimmed.size()) {
                out.ok = false;
                out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "invalid Content-Length header"};
                return out;
            }
            if (out.has_content_length && out.content_length != parsed) {
                // Conflicting duplicate Content-Length — smuggling vector.
                out.ok = false;
                out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "conflicting Content-Length headers"};
                return out;
            }
            out.content_length = parsed;
            out.has_content_length = true;
        } else if (StringUtil::iequals(name, "connection")) {
            out.connection_close |= StringUtil::icontains(value, "close");
            out.connection_keep_alive |= StringUtil::icontains(value, "keep-alive");
        } else if (StringUtil::iequals(name, "transfer-encoding")) {
            has_transfer_encoding = true;
            if (StringUtil::icontains(value, "chunked")) {
                out.is_chunked = true;
            }
        }

        out.headers.emplace(std::string(name), std::string(StringUtil::trim(value)));
    }

    // RFC 7230 §3.3.3: Content-Length and Transfer-Encoding must not
    // both be present; Transfer-Encoding without a final chunked coding
    // is unsupported.
    if (out.has_content_length && has_transfer_encoding) {
        out.ok = false;
        out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "response mixes Content-Length and Transfer-Encoding"};
        return out;
    }
    if (out.minor_version == 0 && out.is_chunked) {
        out.ok = false;
        out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "HTTP/1.0 response uses chunked encoding"};
        return out;
    }
    if (has_transfer_encoding && !out.is_chunked) {
        out.ok = false;
        out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "unsupported Transfer-Encoding (needs chunked)"};
        return out;
    }
    if (out.has_content_length && out.content_length > limits.max_body_bytes) {
        out.ok = false;
        out.error = {ErrorCode::BODY_TOO_LARGE, "Content-Length exceeds limit"};
        return out;
    }
    return out;
}

/// Read a fixed-length body; `buffered` holds bytes already received
/// past the header block.
[[nodiscard]] std::expected<std::string, Error> read_fixed_body(Transport::Stream& stream,
                                                                const size_t total,
                                                                const std::string_view buffered,
                                                                const WireRequest& req,
                                                                std::string& pending) {
    std::string body;
    body.reserve(total);
    const auto available = std::min(buffered.size(), total);
    body.assign(buffered.substr(0, available));
    if (buffered.size() > available) {
        pending.assign(buffered.substr(available));
    }

    while (body.size() < total) {
        std::array<std::uint8_t, READ_CHUNK> buf{};
        const auto needed = std::min(buf.size(), total - body.size());
        auto n = stream.read_some(std::span(buf.data(), needed));
        if (!n) {
            return std::unexpected(map_io_error(n.error(), context(req)));
        }
        if (*n == 0) {
            return std::unexpected(Error{ErrorCode::CONNECTION_LOST, "unexpected EOF in response body"});
        }
        body.append(reinterpret_cast<const char*>(buf.data()), *n);
    }
    return body;
}

/// Read a chunked body (RFC 7230 §4.1).
[[nodiscard]] std::expected<std::string, Error> read_chunked_body(Transport::Stream& stream,
                                                                  const std::string_view buffered,
                                                                  const Limits& limits,
                                                                  const WireRequest& req,
                                                                  std::string& pending) {
    std::vector<char> raw(buffered.begin(), buffered.end());
    std::string body;

    phr_chunked_decoder decoder{};
    decoder.consume_trailer = 1;

    for (;;) {
        auto decoded_size = raw.size();
        const auto ret = phr_decode_chunked(&decoder, raw.data(), &decoded_size);
        if (ret == -1) {
            return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunked body"});
        }
        if (decoded_size > 0) {
            if (body.size() + decoded_size > limits.max_body_bytes) {
                return std::unexpected(Error{ErrorCode::BODY_TOO_LARGE, "response body exceeds limit"});
            }
            body.append(raw.data(), decoded_size);
        }
        if (ret >= 0) {
            pending.assign(raw.data() + decoded_size, static_cast<size_t>(ret));
            return body;
        }

        // phr_decode_chunked has consumed all encoded bytes and retained its
        // state internally; only newly received bytes belong in the next call.
        raw.clear();
        std::array<std::uint8_t, READ_CHUNK> buf{};
        auto n = stream.read_some(buf);
        if (!n) {
            return std::unexpected(map_io_error(n.error(), context(req)));
        }
        if (*n == 0) {
            return std::unexpected(Error{ErrorCode::CONNECTION_LOST, "unexpected EOF inside chunked body"});
        }
        raw.assign(reinterpret_cast<const char*>(buf.data()), reinterpret_cast<const char*>(buf.data()) + *n);
    }
}

/// Read a close-delimited body (no Content-Length, no chunked): the
/// body runs until the peer closes the connection.
[[nodiscard]] std::expected<std::string, Error> read_until_eof(Transport::Stream& stream,
                                                               const std::string_view buffered,
                                                               const Limits& limits,
                                                               const WireRequest& req) {
    std::string body(buffered);
    for (;;) {
        if (body.size() > limits.max_body_bytes) {
            return std::unexpected(Error{ErrorCode::BODY_TOO_LARGE, "response body exceeds limit"});
        }
        std::array<std::uint8_t, READ_CHUNK> buf{};
        auto n = stream.read_some(buf);
        if (!n) {
            if (n.error() == Transport::IoError::CONNECTION_FAILED) {
                return body;  // EOF terminates the body.
            }
            return std::unexpected(map_io_error(n.error(), context(req)));
        }
        if (*n == 0) {
            return body;
        }
        body.append(reinterpret_cast<const char*>(buf.data()), *n);
    }
}

}  // namespace

Error map_io_error(const Transport::IoError err, const std::string_view stage) {
    using enum Transport::IoError;
    switch (err) {
        case CANCELLED:
            return {ErrorCode::CANCELLED, fmt::format("{}: cancelled", stage)};
        case TIMEOUT:
            return {ErrorCode::TIMEOUT, fmt::format("{}: timed out", stage)};
        case CONNECTION_FAILED:
            return {ErrorCode::CONNECTION_LOST, fmt::format("{}: connection lost", stage)};
    }
    return {ErrorCode::CONNECTION_LOST, fmt::format("{}: connection lost", stage)};
}

std::expected<RawResponse, Error> exchange(Transport::Stream& stream,
                                           const WireRequest& req,
                                           const Limits& limits,
                                           std::string& pending) {
    // ── Send ──
    const auto wire = serialize(req);
    const auto* wire_bytes = reinterpret_cast<const std::uint8_t*>(wire.data());
    if (auto sent = stream.send_all(std::span(wire_bytes, wire.size())); !sent) {
        return std::unexpected(map_io_error(sent.error(), context(req)));
    }

    // ── Read headers (incremental parse) ──
    std::string buf = std::move(pending);
    pending.clear();

    HeaderOutcome headers;
    for (;;) {
        headers = parse_headers(buf, limits);
        if (headers.ok) {
            // A 1xx response (except 101 Switching Protocols) is interim;
            // consume it and parse the final response from the same stream.
            if (headers.status >= 100 && headers.status < 200 && headers.status != 101) {
                buf.erase(0, headers.header_end);
                continue;
            }
            break;
        }
        if (!headers.incomplete) {
            return std::unexpected(std::move(headers.error));
        }
        if (buf.size() >= limits.max_header_bytes) {
            return std::unexpected(Error{ErrorCode::HEADERS_TOO_LARGE, "response headers exceed limit"});
        }

        std::array<std::uint8_t, READ_CHUNK> read_buf{};
        const auto capacity = std::min(read_buf.size(), limits.max_header_bytes - buf.size());
        auto n = stream.read_some(std::span(read_buf.data(), capacity));
        if (!n) {
            return std::unexpected(map_io_error(n.error(), context(req)));
        }
        if (*n == 0) {
            return std::unexpected(Error{ErrorCode::CONNECTION_LOST, "connection closed before response headers"});
        }
        buf.append(reinterpret_cast<const char*>(read_buf.data()), *n);
    }

    const auto buffered = std::string_view(buf).substr(headers.header_end);

    // ── Read body per framing ──
    std::expected<std::string, Error> body = std::string{};
    const bool no_body = req.method == Method::HEAD || headers.status == 101 || headers.status == 204 ||
                         headers.status == 304 || (headers.status >= 100 && headers.status < 200);
    if (no_body) {
        pending.assign(buffered);
        body = std::string{};
    } else if (headers.has_content_length) {
        body = read_fixed_body(stream, headers.content_length, buffered, req, pending);
    } else if (headers.is_chunked) {
        body = read_chunked_body(stream, buffered, limits, req, pending);
    } else if (req.method != Method::HEAD) {
        body = read_until_eof(stream, buffered, limits, req);
    }
    if (!body) {
        return std::unexpected(std::move(body.error()));
    }

    SPDLOG_DEBUG("{} {} -> {} ({} bytes)", method_name(req.method), req.target, headers.status, body->size());

    const bool request_closes = std::ranges::any_of(req.headers, [](const auto& header) {
        return StringUtil::iequals(header.first, "connection") && StringUtil::icontains(header.second, "close");
    });
    const bool reusable = !request_closes && !headers.connection_close && headers.status != 101 &&
                          (no_body || headers.has_content_length || headers.is_chunked) &&
                          (headers.minor_version == 1 || headers.connection_keep_alive);

    return RawResponse{
        .status = headers.status,
        .version = headers.minor_version == 0 ? HttpVersion::V1_0 : HttpVersion::V1_1,
        .reusable = reusable,
        .headers = std::move(headers.headers),
        .body = std::move(*body),
    };
}

std::expected<RawResponse, Error> exchange(Transport::Stream& stream,
                                           const WireRequest& req,
                                           const Limits& limits) {
    std::string pending;
    return exchange(stream, req, limits, pending);
}

}  // namespace net::http::protocol
