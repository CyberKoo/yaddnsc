//
// Full HTTP/1.1 request-response exchange over a Transport::Stream.
//
#include "infrastructure/network/http/protocol/exchange.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <expected>
#include <picohttpparser.h>
#include <spdlog/spdlog.h>
#include <yaddnsc/util/format.hpp>
#include <yaddnsc/util/string_util.hpp>

#include "infrastructure/network/http/protocol/wire.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/stream.h"
#include "support/fmt.hpp"
#include "support/string_util.hpp"

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
    std::optional<unsigned> keep_alive_max;
    std::optional<unsigned> keep_alive_timeout;
    std::multimap<std::string, std::string> headers;
    Error error{ErrorCode::RESPONSE_PARSE_FAILED, {}};
};

[[nodiscard]] std::string context(const WireRequest& req) {
    return fmt::format("{} {}", method_name(req.method), req.target);
}

[[nodiscard]] bool is_token(const std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (const auto ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' || ch == '\'' ||
            ch == '*' || ch == '+' || ch == '-' || ch == '.' || ch == '^' || ch == '_' || ch == '`' || ch == '|' ||
            ch == '~') {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool is_field_value(const std::string_view value) noexcept {
    for (const auto ch : value) {
        const auto c = static_cast<unsigned char>(ch);
        if (c != '\t' && (c < 0x20 || c == 0x7f)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_token_list(const std::string_view value) noexcept {
    size_t pos = 0;
    do {
        const auto comma = value.find(',', pos);
        const auto token = StringUtil::trim(value.substr(pos, comma == std::string_view::npos ? comma : comma - pos));
        if (!is_token(token)) {
            return false;
        }
        if (comma == std::string_view::npos) {
            return true;
        }
        pos = comma + 1;
    } while (pos < value.size());
    return false;
}

[[nodiscard]] bool has_token(const std::string_view value, const std::string_view wanted) noexcept {
    size_t pos = 0;
    while (pos < value.size()) {
        const auto comma = value.find(',', pos);
        if (StringUtil::iequals(
                StringUtil::trim(value.substr(pos, comma == std::string_view::npos ? comma : comma - pos)), wanted)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return false;
}

[[nodiscard]] bool has_bare_lf(const std::string_view value) noexcept {
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\n' && (i == 0 || value[i - 1] != '\r')) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool forbidden_trailer(const std::string_view name) noexcept {
    return StringUtil::iequals(name, "content-length") || StringUtil::iequals(name, "transfer-encoding") ||
           StringUtil::iequals(name, "host") || StringUtil::iequals(name, "connection") ||
           StringUtil::iequals(name, "trailer") || StringUtil::iequals(name, "upgrade");
}

[[nodiscard]] std::optional<unsigned> keep_alive_parameter(const std::string_view value,
                                                           const std::string_view wanted) {
    for (const auto& parameter : StringUtil::split(value, ",")) {
        const auto trimmed = StringUtil::trim(parameter);
        const auto eq = trimmed.find('=');
        if (eq == std::string_view::npos || !StringUtil::iequals(StringUtil::trim(trimmed.substr(0, eq)), wanted)) {
            continue;
        }
        unsigned number{};
        const auto field = StringUtil::trim(trimmed.substr(eq + 1));
        const auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), number);
        if (!field.empty() && ec == std::errc{} && ptr == field.data() + field.size()) {
            return number;
        }
    }
    return std::nullopt;
}

struct ChunkedBody {
    std::string body;
    std::multimap<std::string, std::string> trailers;
};

[[nodiscard]] bool valid_chunk_extensions(const std::string_view extensions) noexcept {
    size_t pos = 0;
    while (pos < extensions.size()) {
        if (extensions[pos++] != ';') {
            return false;
        }
        const auto name_start = pos;
        while (pos < extensions.size() && is_token(std::string_view{extensions.data() + pos, 1})) {
            ++pos;
        }
        if (pos == name_start) {
            return false;
        }
        if (pos == extensions.size() || extensions[pos] == ';') {
            continue;
        }
        if (extensions[pos++] != '=') {
            return false;
        }
        if (pos == extensions.size()) {
            return false;
        }
        if (extensions[pos] != '"') {
            const auto value_start = pos;
            while (pos < extensions.size() && is_token(std::string_view{extensions.data() + pos, 1})) {
                ++pos;
            }
            if (pos == value_start) {
                return false;
            }
        } else {
            ++pos;
            bool closed = false;
            while (pos < extensions.size()) {
                const auto c = static_cast<unsigned char>(extensions[pos++]);
                if (c == '"') {
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    if (pos == extensions.size()) {
                        return false;
                    }
                    ++pos;
                } else if (c < 0x20 || c == 0x7f) {
                    return false;
                }
            }
            if (!closed) {
                return false;
            }
        }
        if (pos < extensions.size() && extensions[pos] != ';') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::expected<size_t, Error> parse_chunk_size(const std::string_view line) {
    const auto separator = line.find(';');
    const auto digits = line.substr(0, separator);
    const auto extensions = separator == std::string_view::npos ? std::string_view{} : line.substr(separator);
    if (digits.empty() || !valid_chunk_extensions(extensions)) {
        return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunk size or extension"});
    }
    size_t size{};
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), size, 16);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
        return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "invalid chunk size"});
    }
    return size;
}

[[nodiscard]] std::expected<std::multimap<std::string, std::string>, Error> parse_trailers(const std::string_view data,
                                                                                           std::string& pending) {
    const bool empty = data.starts_with("\r\n");
    const auto end = empty ? 0 : data.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "incomplete chunked trailers"});
    }

    std::multimap<std::string, std::string> trailers;
    if (!empty) {
        std::array<phr_header, MAX_HEADERS> fields{};
        size_t field_count = fields.size();
        const auto parsed = phr_parse_headers(data.data(), end + 4, fields.data(), &field_count, 0);
        if (parsed < 0 || static_cast<size_t>(parsed) != end + 4) {
            return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunked trailers"});
        }
        for (size_t i = 0; i < field_count; ++i) {
            const auto name = std::string_view(fields[i].name, fields[i].name_len);
            const auto value = StringUtil::trim(std::string_view(fields[i].value, fields[i].value_len));
            if (!is_token(name) || forbidden_trailer(name) || !is_field_value(value)) {
                return std::unexpected(
                    Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed, forbidden, or unsafe chunked trailer"});
            }
            trailers.emplace(name, value);
        }
    }
    pending.assign(data.substr(empty ? 2 : end + 4));
    return trailers;
}

[[nodiscard]] bool is_request_target(const std::string_view target) noexcept {
    if (target == "*") {
        return true;
    }
    if (target.empty() || target.front() != '/') {
        return false;
    }
    return std::ranges::none_of(target, [](const char ch) {
        const auto c = static_cast<unsigned char>(ch);
        return c <= 0x20 || c == 0x7f;
    });
}

[[nodiscard]] std::optional<Error> validate_wire_request(const WireRequest& req) {
    if (!is_request_target(req.target)) {
        return Error{ErrorCode::INVALID_REQUEST, "invalid HTTP request target"};
    }

    size_t hosts = 0;
    size_t connections = 0;
    std::optional<size_t> content_length;
    for (const auto& [name, value] : req.headers) {
        if (!is_token(name) || !is_field_value(value) ||
            (StringUtil::iequals(name, "connection") && !valid_token_list(value))) {
            return Error{ErrorCode::INVALID_REQUEST, "invalid HTTP request header"};
        }
        if (StringUtil::iequals(name, "upgrade") ||
            (StringUtil::iequals(name, "connection") && has_token(value, "upgrade"))) {
            return Error{ErrorCode::UNSUPPORTED_PROTOCOL, "HTTP protocol upgrade is not supported"};
        }
        if (StringUtil::iequals(name, "transfer-encoding") || StringUtil::iequals(name, "trailer")) {
            return Error{ErrorCode::INVALID_REQUEST, "request transfer coding and trailers are not supported"};
        }
        if (StringUtil::iequals(name, "host")) {
            ++hosts;
        }
        if (StringUtil::iequals(name, "connection")) {
            ++connections;
        }
        if (StringUtil::iequals(name, "content-length")) {
            if (content_length) {
                return Error{ErrorCode::INVALID_REQUEST, "conflicting HTTP framing or routing headers"};
            }
            size_t parsed{};
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size()) {
                return Error{ErrorCode::INVALID_REQUEST, "invalid HTTP Content-Length"};
            }
            content_length = parsed;
        }
    }
    if (hosts > 1 || connections > 1 || (content_length && *content_length != (req.body ? req.body->size() : 0))) {
        return Error{ErrorCode::INVALID_REQUEST, "conflicting HTTP framing or routing headers"};
    }
    return std::nullopt;
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
        if (!is_token(name) || !is_field_value(value)) {
            out.ok = false;
            out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "invalid response header"};
            return out;
        }

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
            if (!valid_token_list(value)) {
                out.ok = false;
                out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "invalid Connection header"};
                return out;
            }
            out.connection_close |= has_token(value, "close");
            out.connection_keep_alive |= has_token(value, "keep-alive");
        } else if (StringUtil::iequals(name, "keep-alive")) {
            if (const auto max = keep_alive_parameter(value, "max")) {
                out.keep_alive_max = max;
            }
            if (const auto timeout = keep_alive_parameter(value, "timeout")) {
                out.keep_alive_timeout = timeout;
            }
        } else if (StringUtil::iequals(name, "transfer-encoding")) {
            if (has_transfer_encoding) {
                out.ok = false;
                out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "multiple Transfer-Encoding headers"};
                return out;
            }
            has_transfer_encoding = true;
            // This client deliberately implements only the single `chunked`
            // coding. Accepting `gzip, chunked` without decoding gzip corrupts
            // the representation and is an HTTP message-smuggling hazard.
            if (!StringUtil::iequals(StringUtil::trim(value), "chunked")) {
                out.ok = false;
                out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "unsupported Transfer-Encoding"};
                return out;
            }
            out.is_chunked = true;
        }

        out.headers.emplace(std::string(name), std::string(StringUtil::trim(value)));
    }

    // RFC 7230 §3.3.3: Content-Length and Transfer-Encoding must not
    // both be present; Transfer-Encoding without a final chunked coding
    // is unsupported.
    if (out.connection_close && out.connection_keep_alive) {
        out.ok = false;
        out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "conflicting Connection directives"};
        return out;
    }
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
                                                                std::string& pending,
                                                                const Utils::CancellationToken& token) {
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
        auto n = stream.read_some(std::span(buf.data(), needed), token);
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

/// Read a chunked body (RFC 9112 §7.1), including strict extensions and trailers.
[[nodiscard]] std::expected<ChunkedBody, Error> read_chunked_body(Transport::Stream& stream,
                                                                  const std::string_view buffered,
                                                                  const Limits& limits,
                                                                  const WireRequest& req,
                                                                  std::string& pending,
                                                                  const Utils::CancellationToken& token) {
    std::string raw{buffered};
    ChunkedBody result;
    const auto read_more = [&]() -> std::expected<void, Error> {
        std::array<std::uint8_t, READ_CHUNK> buf{};
        auto n = stream.read_some(buf, token);
        if (!n) {
            return std::unexpected(map_io_error(n.error(), context(req)));
        }
        if (*n == 0) {
            return std::unexpected(Error{ErrorCode::CONNECTION_LOST, "unexpected EOF inside chunked body"});
        }
        raw.append(reinterpret_cast<const char*>(buf.data()), *n);
        return {};
    };

    for (;;) {
        size_t line_end{};
        while ((line_end = raw.find("\r\n")) == std::string::npos) {
            if (has_bare_lf(raw)) {
                return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunk delimiter"});
            }
            if (raw.size() >= limits.max_header_bytes) {
                return std::unexpected(Error{ErrorCode::HEADERS_TOO_LARGE, "chunk metadata exceeds limit"});
            }
            if (auto more = read_more(); !more) {
                return std::unexpected(std::move(more.error()));
            }
        }
        auto chunk_size = parse_chunk_size(std::string_view(raw).substr(0, line_end));
        if (!chunk_size) {
            return std::unexpected(std::move(chunk_size.error()));
        }
        raw.erase(0, line_end + 2);

        if (*chunk_size == 0) {
            while (!raw.starts_with("\r\n") && raw.find("\r\n\r\n") == std::string::npos) {
                if (has_bare_lf(raw)) {
                    return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunked trailers"});
                }
                if (raw.size() >= limits.max_header_bytes) {
                    return std::unexpected(Error{ErrorCode::HEADERS_TOO_LARGE, "chunked trailers exceed limit"});
                }
                if (auto more = read_more(); !more) {
                    return std::unexpected(std::move(more.error()));
                }
            }
            const auto trailers_end = raw.starts_with("\r\n") ? size_t{2} : raw.find("\r\n\r\n") + 4;
            if (trailers_end > limits.max_header_bytes) {
                return std::unexpected(Error{ErrorCode::HEADERS_TOO_LARGE, "chunked trailers exceed limit"});
            }
            auto trailers = parse_trailers(raw, pending);
            if (!trailers) {
                return std::unexpected(std::move(trailers.error()));
            }
            result.trailers = std::move(*trailers);
            return result;
        }

        if (*chunk_size > limits.max_body_bytes - result.body.size()) {
            return std::unexpected(Error{ErrorCode::BODY_TOO_LARGE, "response body exceeds limit"});
        }
        while (raw.size() < *chunk_size || raw.size() - *chunk_size < 2) {
            if (auto more = read_more(); !more) {
                return std::unexpected(std::move(more.error()));
            }
        }
        if (raw[*chunk_size] != '\r' || raw[*chunk_size + 1] != '\n') {
            return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunk delimiter"});
        }
        result.body.append(raw.data(), *chunk_size);
        raw.erase(0, *chunk_size + 2);
    }
}

/// Read a close-delimited body (no Content-Length, no chunked): the
/// body runs until the peer closes the connection.
[[nodiscard]] std::expected<std::string, Error> read_until_eof(Transport::Stream& stream,
                                                               const std::string_view buffered,
                                                               const Limits& limits,
                                                               const WireRequest& req,
                                                               const Utils::CancellationToken& token) {
    std::string body(buffered);
    for (;;) {
        if (body.size() > limits.max_body_bytes) {
            return std::unexpected(Error{ErrorCode::BODY_TOO_LARGE, "response body exceeds limit"});
        }
        std::array<std::uint8_t, READ_CHUNK> buf{};
        auto n = stream.read_some(buf, token);
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
                                           std::string& pending,
                                           const Utils::CancellationToken& token) {
    // ── Send ──
    if (const auto invalid = validate_wire_request(req)) {
        return std::unexpected(*invalid);
    }
    const auto wire = serialize(req);
    const auto* wire_bytes = reinterpret_cast<const std::uint8_t*>(wire.data());
    if (auto sent = stream.send_all(std::span(wire_bytes, wire.size()), token); !sent) {
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
                if (headers.has_content_length || headers.is_chunked) {
                    return std::unexpected(
                        Error{ErrorCode::RESPONSE_PARSE_FAILED, "interim response has body framing"});
                }
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
        auto n = stream.read_some(std::span(read_buf.data(), capacity), token);
        if (!n) {
            return std::unexpected(map_io_error(n.error(), context(req)));
        }
        if (*n == 0) {
            return std::unexpected(Error{ErrorCode::CONNECTION_LOST, "connection closed before response headers"});
        }
        buf.append(reinterpret_cast<const char*>(read_buf.data()), *n);
    }

    const auto buffered = std::string_view(buf).substr(headers.header_end);

    if (headers.status == 101) {
        return std::unexpected(Error{ErrorCode::UNSUPPORTED_PROTOCOL, "HTTP protocol upgrade is not supported"});
    }
    // ── Read body per framing ──
    std::expected<std::string, Error> body = std::string{};
    std::multimap<std::string, std::string> trailers;
    const bool no_body = req.method == Method::HEAD || headers.status == 101 || headers.status == 204 ||
                         headers.status == 205 || headers.status == 304 ||
                         (headers.status >= 100 && headers.status < 200);
    if (no_body) {
        if (headers.status == 205) {
            if (headers.has_content_length && headers.content_length != 0) {
                return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "205 response has a non-empty body"});
            }
            if (headers.is_chunked) {
                auto chunked = read_chunked_body(stream, buffered, limits, req, pending, token);
                if (!chunked) {
                    return std::unexpected(std::move(chunked.error()));
                }
                if (!chunked->body.empty()) {
                    return std::unexpected(
                        Error{ErrorCode::RESPONSE_PARSE_FAILED, "205 response has a non-empty body"});
                }
                trailers = std::move(chunked->trailers);
                body = std::string{};
            } else if (headers.has_content_length) {
                pending.assign(buffered);
                body = std::string{};
            } else if (headers.connection_close) {
                body = read_until_eof(stream, buffered, limits, req, token);
                if (body && !body->empty()) {
                    return std::unexpected(
                        Error{ErrorCode::RESPONSE_PARSE_FAILED, "205 response has a non-empty body"});
                }
            } else {
                return std::unexpected(
                    Error{ErrorCode::RESPONSE_PARSE_FAILED, "205 response lacks zero-length framing"});
            }
        } else {
            pending.assign(buffered);
            body = std::string{};
        }
    } else if (headers.has_content_length) {
        body = read_fixed_body(stream, headers.content_length, buffered, req, pending, token);
    } else if (headers.is_chunked) {
        auto chunked = read_chunked_body(stream, buffered, limits, req, pending, token);
        if (!chunked) {
            return std::unexpected(std::move(chunked.error()));
        }
        body = std::move(chunked->body);
        trailers = std::move(chunked->trailers);
    } else {
        body = read_until_eof(stream, buffered, limits, req, token);
    }
    if (!body) {
        return std::unexpected(std::move(body.error()));
    }

    SPDLOG_DEBUG("{} {} -> {} ({} bytes)", method_name(req.method), req.target, headers.status, body->size());

    const bool request_closes = std::ranges::any_of(req.headers, [](const auto& header) {
        return StringUtil::iequals(header.first, "connection") && has_token(header.second, "close");
    });
    const bool reusable = !request_closes && !headers.connection_close && headers.status != 101 &&
                          (no_body || headers.has_content_length || headers.is_chunked) &&
                          (headers.minor_version == 1 || headers.connection_keep_alive);

    return RawResponse{
        .status = headers.status,
        .version = headers.minor_version == 0 ? HttpVersion::V1_0 : HttpVersion::V1_1,
        .reusable = reusable,
        .keep_alive_max = headers.keep_alive_max,
        .keep_alive_timeout = headers.keep_alive_timeout,
        .headers = std::move(headers.headers),
        .trailers = std::move(trailers),
        .body = std::move(*body),
    };
}

std::expected<RawResponse, Error> exchange(Transport::Stream& stream,
                                           const WireRequest& req,
                                           const Limits& limits,
                                           const Utils::CancellationToken& token) {
    std::string pending;
    return exchange(stream, req, limits, pending, token);
}

}  // namespace net::http::protocol
