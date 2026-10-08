//
// http — full HTTP/1.x request-response exchange over a net::Stream.
//

#include "exchange.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <picohttpparser.h>
#include <spdlog/spdlog.h>

#include "infrastructure/net/http/protocol/field_chars.hpp"
#include "infrastructure/net/http/protocol/read_window.h"
#include "infrastructure/net/http/protocol/wire.h"
#include "infrastructure/net/http/wire_request.h"
#include "support/fmt.hpp"
#include "support/string_util.hpp"

namespace http::protocol {

namespace {

/// Maximum number of response header fields parsed per message.
constexpr std::size_t MAX_HEADERS = 64;

/// Read size for headers and body transfers.
constexpr std::size_t READ_CHUNK = 4096;

/// Result of one incremental header-parse attempt.
struct HeaderOutcome {
    bool ok = false;             ///< Headers fully parsed.
    bool incomplete = false;     ///< Need more bytes.
    int status = 0;              ///< Status code (valid when ok).
    int minor_version = 1;       ///< HTTP/1.x minor version.
    std::size_t header_end = 0;  ///< Offset of the body start (valid when ok).
    std::size_t content_length = 0;
    bool has_content_length = false;
    bool is_chunked = false;
    bool connection_close = false;
    bool connection_keep_alive = false;
    std::optional<unsigned> keep_alive_max;
    std::optional<unsigned> keep_alive_timeout;
    std::multimap<std::string, std::string> headers;
    Error error{ErrorCode::RESPONSE_PARSE_FAILED, {}};
};

/// The request target without its query string.
///
/// A query string can carry provider credentials, so neither diagnostics nor
/// log lines may include it.
[[nodiscard]] std::string_view request_path(const std::string_view target) noexcept {
    return target.substr(0, target.find('?'));
}

/// "METHOD /path" for error messages — never the query string.
[[nodiscard]] std::string stage_context(const WireRequest& request) {
    return fmt::format("{} {}", method_name(request.method), request_path(request.target));
}

[[nodiscard]] bool valid_token_list(const std::string_view value) noexcept {
    std::size_t pos = 0;
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
    std::size_t pos = 0;
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
    for (std::size_t i = 0; i < value.size(); ++i) {
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
        const auto equals = trimmed.find('=');
        if (equals == std::string_view::npos ||
            !StringUtil::iequals(StringUtil::trim(trimmed.substr(0, equals)), wanted)) {
            continue;
        }
        unsigned number{};
        const auto field = StringUtil::trim(trimmed.substr(equals + 1));
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
    std::size_t pos = 0;
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
                const auto character = static_cast<unsigned char>(extensions[pos++]);
                if (character == '"') {
                    closed = true;
                    break;
                }
                if (character == '\\') {
                    if (pos == extensions.size()) {
                        return false;
                    }
                    ++pos;
                } else if (character < 0x20 || character == 0x7f) {
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

[[nodiscard]] std::expected<std::size_t, Error> parse_chunk_size(const std::string_view line) {
    const auto separator = line.find(';');
    const auto digits = line.substr(0, separator);
    const auto extensions = separator == std::string_view::npos ? std::string_view{} : line.substr(separator);
    if (digits.empty() || !valid_chunk_extensions(extensions)) {
        return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunk size or extension"});
    }
    std::size_t size{};
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), size, 16);
    if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
        return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "invalid chunk size"});
    }
    return size;
}

/// Parse the trailer section at the front of `bytes`, leaving the remainder in
/// `rest`.
[[nodiscard]] std::expected<std::multimap<std::string, std::string>, Error> parse_trailers(const std::string_view bytes,
                                                                                           std::string& rest) {
    const bool empty = bytes.starts_with("\r\n");
    const auto end = empty ? 0 : bytes.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "incomplete chunked trailers"});
    }

    std::multimap<std::string, std::string> trailers;
    if (!empty) {
        std::array<phr_header, MAX_HEADERS> fields{};
        std::size_t field_count = fields.size();
        const auto parsed = phr_parse_headers(bytes.data(), end + 4, fields.data(), &field_count, 0);
        if (parsed < 0 || static_cast<std::size_t>(parsed) != end + 4) {
            return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunked trailers"});
        }
        for (std::size_t i = 0; i < field_count; ++i) {
            const auto name = std::string_view(fields[i].name, fields[i].name_len);
            const auto value = StringUtil::trim(std::string_view(fields[i].value, fields[i].value_len));
            if (!is_token(name) || forbidden_trailer(name) || !is_field_value(value)) {
                return std::unexpected(
                    Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed, forbidden, or unsafe chunked trailer"});
            }
            trailers.emplace(name, value);
        }
    }
    rest.assign(bytes.substr(empty ? 2 : end + 4));
    return trailers;
}

[[nodiscard]] bool is_request_target(const std::string_view target) noexcept {
    if (target == "*") {
        return true;
    }
    if (target.empty() || target.front() != '/') {
        return false;
    }
    return std::ranges::none_of(target, [](const char character) {
        const auto value = static_cast<unsigned char>(character);
        return value <= 0x20 || value == 0x7f;
    });
}

[[nodiscard]] std::optional<Error> validate_wire_request(const WireRequest& request) {
    if (!is_request_target(request.target)) {
        return Error{ErrorCode::INVALID_REQUEST, "invalid HTTP request target"};
    }

    std::size_t hosts = 0;
    std::size_t connections = 0;
    std::optional<std::size_t> content_length;
    for (const auto& [name, value] : request.headers) {
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
            std::size_t parsed{};
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (value.empty() || ec != std::errc{} || ptr != value.data() + value.size()) {
                return Error{ErrorCode::INVALID_REQUEST, "invalid HTTP Content-Length"};
            }
            content_length = parsed;
        }
    }
    if (hosts > 1 || connections > 1 ||
        (content_length && *content_length != (request.body ? request.body->size() : 0))) {
        return Error{ErrorCode::INVALID_REQUEST, "conflicting HTTP framing or routing headers"};
    }
    return std::nullopt;
}

/// Parse the accumulated bytes as response headers in a single pass.
[[nodiscard]] HeaderOutcome parse_headers(const std::string_view buffer, const Limits& limits) {
    HeaderOutcome out;

    int status = 0;
    int minor_version = 0;
    const char* message = nullptr;
    std::size_t message_length = 0;
    std::array<phr_header, MAX_HEADERS> headers{};
    std::size_t header_count = MAX_HEADERS;

    const auto parsed = phr_parse_response(buffer.data(), buffer.size(), &minor_version, &status, &message,
                                           &message_length, headers.data(), &header_count, 0);
    if (parsed == -2) {
        out.incomplete = true;
        return out;
    }
    if (parsed == -1) {
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
    out.header_end = static_cast<std::size_t>(parsed);

    bool has_transfer_encoding = false;
    for (std::size_t i = 0; i < header_count; ++i) {
        const auto name = std::string_view(headers[i].name, headers[i].name_len);
        const auto value = std::string_view(headers[i].value, headers[i].value_len);
        if (!is_token(name) || !is_field_value(value)) {
            out.ok = false;
            out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "invalid response header"};
            return out;
        }

        if (StringUtil::iequals(name, "content-length")) {
            const auto trimmed = StringUtil::trim(value);
            std::size_t size = 0;
            const auto [ptr, ec] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), size);
            if (trimmed.empty() || ec != std::errc{} || ptr != trimmed.data() + trimmed.size()) {
                out.ok = false;
                out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "invalid Content-Length header"};
                return out;
            }
            if (out.has_content_length && out.content_length != size) {
                // Conflicting duplicate Content-Length — a smuggling vector.
                out.ok = false;
                out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "conflicting Content-Length headers"};
                return out;
            }
            out.content_length = size;
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
            // Only the single `chunked` coding is implemented: accepting
            // `gzip, chunked` without decoding gzip corrupts the representation
            // and is a smuggling hazard.
            if (!StringUtil::iequals(StringUtil::trim(value), "chunked")) {
                out.ok = false;
                out.error = {ErrorCode::RESPONSE_PARSE_FAILED, "unsupported Transfer-Encoding"};
                return out;
            }
            out.is_chunked = true;
        }

        out.headers.emplace(std::string(name), std::string(StringUtil::trim(value)));
    }

    // RFC 7230 §3.3.3: Content-Length and Transfer-Encoding are mutually
    // exclusive, and Transfer-Encoding must end in `chunked`.
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

/// Read more bytes into `window`. A peer close mid-message is CONNECTION_LOST.
[[nodiscard]] coro::Task<std::expected<void, Error>> read_more(net::Stream& stream, ReadWindow& window,
                                                               const std::string_view context) {
    std::array<std::uint8_t, READ_CHUNK> buffer{};
    auto read = co_await stream.read_some(buffer);
    if (!read) {
        co_return std::unexpected(map_io_error(read.error(), context));
    }
    if (*read == 0) {
        co_return std::unexpected(Error{ErrorCode::CONNECTION_LOST, fmt::format("{}: unexpected EOF", context)});
    }
    window.append(reinterpret_cast<const char*>(buffer.data()), *read);
    co_return {};
}

/// Read a fixed-length body from the buffered prefix plus the stream.
[[nodiscard]] coro::Task<std::expected<std::string, Error>> read_fixed_body(net::Stream& stream, ReadWindow& window,
                                                                            const std::size_t total,
                                                                            const std::string_view context) {
    std::string body;
    body.reserve(total);

    const auto from_buffer = std::min(window.size(), total);
    body.assign(window.view().substr(0, from_buffer));
    window.consume(from_buffer);

    while (body.size() < total) {
        const auto needed = std::min(READ_CHUNK, total - body.size());
        std::array<std::uint8_t, READ_CHUNK> buffer{};
        auto read = co_await stream.read_some(std::span(buffer.data(), needed));
        if (!read) {
            co_return std::unexpected(map_io_error(read.error(), context));
        }
        if (*read == 0) {
            co_return std::unexpected(
                Error{ErrorCode::CONNECTION_LOST, fmt::format("{}: unexpected EOF in response body", context)});
        }
        body.append(reinterpret_cast<const char*>(buffer.data()), *read);
    }
    co_return body;
}

/// Read a chunked body (RFC 9112 §7.1) with strict extensions and trailers.
[[nodiscard]] coro::Task<std::expected<ChunkedBody, Error>> read_chunked_body(net::Stream& stream, ReadWindow& window,
                                                                              const Limits& limits,
                                                                              const std::string_view context) {
    ChunkedBody result;
    for (;;) {
        // Chunk-size line.
        std::size_t line_end{};
        while ((line_end = window.view().find("\r\n")) == std::string_view::npos) {
            if (has_bare_lf(window.view())) {
                co_return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunk delimiter"});
            }
            if (window.size() >= limits.max_header_bytes) {
                co_return std::unexpected(Error{ErrorCode::HEADERS_TOO_LARGE, "chunk metadata exceeds limit"});
            }
            if (auto more = co_await read_more(stream, window, context); !more) {
                co_return std::unexpected(std::move(more.error()));
            }
        }
        auto chunk_size = parse_chunk_size(window.view().substr(0, line_end));
        if (!chunk_size) {
            co_return std::unexpected(std::move(chunk_size.error()));
        }
        window.consume(line_end + 2);

        if (*chunk_size == 0) {
            // Trailer section, then the terminating CRLF.
            while (!window.view().starts_with("\r\n") && window.view().find("\r\n\r\n") == std::string_view::npos) {
                if (has_bare_lf(window.view())) {
                    co_return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunked trailers"});
                }
                if (window.size() >= limits.max_header_bytes) {
                    co_return std::unexpected(Error{ErrorCode::HEADERS_TOO_LARGE, "chunked trailers exceed limit"});
                }
                if (auto more = co_await read_more(stream, window, context); !more) {
                    co_return std::unexpected(std::move(more.error()));
                }
            }
            const auto trailers_end =
                window.view().starts_with("\r\n") ? std::size_t{2} : window.view().find("\r\n\r\n") + 4;
            if (trailers_end > limits.max_header_bytes) {
                co_return std::unexpected(Error{ErrorCode::HEADERS_TOO_LARGE, "chunked trailers exceed limit"});
            }
            std::string rest;
            auto trailers = parse_trailers(window.view().substr(0, trailers_end), rest);
            if (!trailers) {
                co_return std::unexpected(std::move(trailers.error()));
            }
            result.trailers = std::move(*trailers);
            window.consume(trailers_end);
            co_return result;
        }

        if (*chunk_size > limits.max_body_bytes - result.body.size()) {
            co_return std::unexpected(Error{ErrorCode::BODY_TOO_LARGE, "response body exceeds limit"});
        }
        while (window.size() < *chunk_size || window.size() - *chunk_size < 2) {
            if (auto more = co_await read_more(stream, window, context); !more) {
                co_return std::unexpected(std::move(more.error()));
            }
        }
        const auto chunk = window.view();
        if (chunk[*chunk_size] != '\r' || chunk[*chunk_size + 1] != '\n') {
            co_return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "malformed chunk delimiter"});
        }
        result.body.append(chunk.data(), *chunk_size);
        window.consume(*chunk_size + 2);
    }
}

/// Read a close-delimited body: the body runs until the peer closes.
[[nodiscard]] coro::Task<std::expected<std::string, Error>> read_until_eof(net::Stream& stream, ReadWindow& window,
                                                                           const Limits& limits,
                                                                           const std::string_view context) {
    std::string body{window.view()};
    window.consume(window.size());

    for (;;) {
        if (body.size() > limits.max_body_bytes) {
            co_return std::unexpected(Error{ErrorCode::BODY_TOO_LARGE, "response body exceeds limit"});
        }
        std::array<std::uint8_t, READ_CHUNK> buffer{};
        auto read = co_await stream.read_some(buffer);
        if (!read) {
            if (read.error() == net::IoError::CONNECTION_FAILED) {
                co_return body;  // EOF terminates the body.
            }
            co_return std::unexpected(map_io_error(read.error(), context));
        }
        if (*read == 0) {
            co_return body;
        }
        body.append(reinterpret_cast<const char*>(buffer.data()), *read);
    }
}

}  // namespace

coro::Task<std::expected<RawResponse, Error>> exchange(net::Stream& stream, const WireRequest& request,
                                                       const Limits& limits, std::string& pending) {
    const std::string context = stage_context(request);

    // ── Send ──
    if (const auto invalid = validate_wire_request(request)) {
        co_return std::unexpected(*invalid);
    }
    const auto wire = serialize(request);
    const auto* wire_bytes = reinterpret_cast<const std::uint8_t*>(wire.data());
    if (auto sent = co_await stream.send_all(std::span(wire_bytes, wire.size())); !sent) {
        co_return std::unexpected(map_io_error(sent.error(), context));
    }

    // ── Read headers (incremental parse) ──
    ReadWindow window{pending};
    pending.clear();

    HeaderOutcome headers;
    std::size_t interim_responses = 0;
    for (;;) {
        headers = parse_headers(window.view(), limits);
        if (headers.ok) {
            // A 1xx response other than 101 is interim: consume it and parse the
            // final response from the same stream. 101 never reaches body
            // framing (rejected below), so the interim branch stops here.
            if (headers.status >= 100 && headers.status < 200 && headers.status != 101) {
                if (headers.has_content_length || headers.is_chunked) {
                    co_return std::unexpected(
                        Error{ErrorCode::RESPONSE_PARSE_FAILED, "interim response has body framing"});
                }
                if (interim_responses >= limits.max_interim_responses) {
                    co_return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "too many interim responses"});
                }
                ++interim_responses;
                window.consume(headers.header_end);
                continue;
            }
            break;
        }
        if (!headers.incomplete) {
            co_return std::unexpected(std::move(headers.error));
        }
        if (window.size() >= limits.max_header_bytes) {
            co_return std::unexpected(Error{ErrorCode::HEADERS_TOO_LARGE, "response headers exceed limit"});
        }

        std::array<std::uint8_t, READ_CHUNK> read_buffer{};
        const auto capacity = std::min(read_buffer.size(), limits.max_header_bytes - window.size());
        auto read = co_await stream.read_some(std::span(read_buffer.data(), capacity));
        if (!read) {
            co_return std::unexpected(map_io_error(read.error(), context));
        }
        if (*read == 0) {
            co_return std::unexpected(Error{ErrorCode::CONNECTION_LOST, "connection closed before response headers"});
        }
        window.append(reinterpret_cast<const char*>(read_buffer.data()), *read);
    }

    window.consume(headers.header_end);

    if (headers.status == 101) {
        co_return std::unexpected(Error{ErrorCode::UNSUPPORTED_PROTOCOL, "HTTP protocol upgrade is not supported"});
    }

    // ── Read the body per framing ──
    // Only these three statuses and HEAD carry no body here: 1xx was consumed
    // above and 101 was just rejected, so neither can reach this decision.
    const bool no_body =
        request.method == Method::HEAD || headers.status == 204 || headers.status == 205 || headers.status == 304;

    std::expected<std::string, Error> body = std::string{};
    std::multimap<std::string, std::string> trailers;
    if (no_body) {
        if (headers.status == 205) {
            // 205 must announce a zero-length body; consume whatever framing it
            // used so the window stays aligned for the next exchange.
            if (headers.has_content_length && headers.content_length != 0) {
                co_return std::unexpected(Error{ErrorCode::RESPONSE_PARSE_FAILED, "205 response has a non-empty body"});
            }
            if (headers.is_chunked) {
                auto chunked = co_await read_chunked_body(stream, window, limits, context);
                if (!chunked) {
                    co_return std::unexpected(std::move(chunked.error()));
                }
                if (!chunked->body.empty()) {
                    co_return std::unexpected(
                        Error{ErrorCode::RESPONSE_PARSE_FAILED, "205 response has a non-empty body"});
                }
                trailers = std::move(chunked->trailers);
                body = std::string{};
            } else if (headers.has_content_length) {
                body = std::string{};
            } else if (headers.connection_close) {
                body = co_await read_until_eof(stream, window, limits, context);
                if (body && !body->empty()) {
                    co_return std::unexpected(
                        Error{ErrorCode::RESPONSE_PARSE_FAILED, "205 response has a non-empty body"});
                }
            } else {
                co_return std::unexpected(
                    Error{ErrorCode::RESPONSE_PARSE_FAILED, "205 response lacks zero-length framing"});
            }
        } else {
            body = std::string{};
        }
    } else if (headers.has_content_length) {
        body = co_await read_fixed_body(stream, window, headers.content_length, context);
    } else if (headers.is_chunked) {
        auto chunked = co_await read_chunked_body(stream, window, limits, context);
        if (!chunked) {
            co_return std::unexpected(std::move(chunked.error()));
        }
        body = std::move(chunked->body);
        trailers = std::move(chunked->trailers);
    } else {
        body = co_await read_until_eof(stream, window, limits, context);
    }
    if (!body) {
        co_return std::unexpected(std::move(body.error()));
    }

    // Leftover bytes belong to the next response on a keep-alive connection.
    pending = window.take_rest();

    SPDLOG_DEBUG("{} {} -> {} ({} bytes)", method_name(request.method), request_path(request.target), headers.status,
                 body->size());

    const bool request_closes = std::ranges::any_of(request.headers, [](const auto& header) {
        return StringUtil::iequals(header.first, "connection") && has_token(header.second, "close");
    });
    const bool reusable = !request_closes && !headers.connection_close &&
                          (no_body || headers.has_content_length || headers.is_chunked) &&
                          (headers.minor_version == 1 || headers.connection_keep_alive);

    co_return RawResponse{
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

coro::Task<std::expected<RawResponse, Error>> exchange(net::Stream& stream, const WireRequest& request,
                                                       const Limits& limits) {
    std::string pending;
    co_return co_await exchange(stream, request, limits, pending);
}

}  // namespace http::protocol
