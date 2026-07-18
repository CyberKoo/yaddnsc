//
// Created by Kotarou on 2026/7/18.
//
#include "body_parser.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <expected>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace {

/// Internal read adapter that first consumes from a memory buffer,
/// then falls back to the transport stream.
/// This avoids splitting the buffered-body / network-read logic across
/// every call site inside the chunked decoder.
class BufferedStream {
public:
    explicit BufferedStream(Transport::Stream &stream,
                             const Utils::CancellationToken &cancel_token,
                             std::span<const char> body_buf) noexcept
        : stream_(stream), cancel_token_(cancel_token),
          body_buf_(body_buf), buf_used_(0) {}

    [[nodiscard]] std::expected<void, Http::Error> read_exact(std::span<std::uint8_t> dst) {
        size_t need = dst.size();
        while (need > 0) {
            // Consume from buffer first.
            if (buf_used_ < body_buf_.size()) {
                const size_t avail = body_buf_.size() - buf_used_;
                const size_t take = (std::min)(need, avail);
                auto src = body_buf_.subspan(buf_used_, take);
                std::ranges::copy_n(
                    reinterpret_cast<const std::uint8_t *>(src.data()),
                    static_cast<std::ptrdiff_t>(take), dst.data());
                buf_used_ += take;
                dst = dst.subspan(take);
                need -= take;
            } else {
                // Fall back to transport read.
                auto r = stream_.read_exact(dst, cancel_token_);
                if (!r) {
                    return std::unexpected(map_read_error(r.error()));
                }
                need = 0;
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<std::uint8_t, Http::Error> read_byte() {
        std::uint8_t b;
        auto r = read_exact(std::span(&b, 1));
        if (!r) return std::unexpected(r.error());
        return b;
    }

private:
    static Http::Error map_read_error(Transport::IoError e) noexcept {
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

    Transport::Stream &stream_;
    const Utils::CancellationToken &cancel_token_;
    std::span<const char> body_buf_;
    size_t buf_used_;
};

} // anonymous namespace

namespace Http {

std::expected<std::vector<std::uint8_t>, Error> read_body(
    Transport::Stream &stream,
    const ResponseHeaders &headers,
    std::span<const char> header_buf,
    size_t max_body_size,
    const Utils::CancellationToken &cancel_token) {

    const size_t body_buffered = header_buf.size() > headers.header_end
                                    ? header_buf.size() - headers.header_end
                                    : 0;

    // ── Fixed-length body (Content-Length) ──
    if (headers.has_content_length) {
        std::vector<std::uint8_t> body;
        body.reserve(headers.content_length);

        // Copy any already-buffered body data.
        if (body_buffered > 0) {
            auto src = header_buf.subspan(headers.header_end, body_buffered);
            const auto copy_size = (std::min)(body_buffered, headers.content_length);
            body.insert(body.end(),
                        reinterpret_cast<const std::uint8_t *>(src.data()),
                        reinterpret_cast<const std::uint8_t *>(src.data() + copy_size));
        }

        // Read remaining from transport stream.
        if (body_buffered < headers.content_length) {
            body.resize(headers.content_length);
            auto *dst = body.data() + body_buffered;
            const auto remaining = headers.content_length - body_buffered;
            auto st = stream.read_exact(std::span(dst, remaining), cancel_token);
            if (!st) {
                switch (st.error()) {
                case Transport::IoError::CANCELLED:
                    return std::unexpected(Error::CANCELLED);
                default:
                    return std::unexpected(Error::CONNECTION_FAILED);
                }
            }
        }

        return body;
    }

    // ── Chunked transfer encoding ──
    // Wrap the buffered body data + transport stream into a single logical
    // source so the chunked decoder does not have to worry about buffering.
    const auto body_buf = header_buf.subspan(headers.header_end, body_buffered);
    BufferedStream buffered(stream, cancel_token, body_buf);

    std::vector<std::uint8_t> body;
    body.reserve(max_body_size);
    size_t body_size = 0;

    // Read a CRLF-terminated line (without the trailing CRLF).
    auto read_line = [&](std::string &line) -> std::expected<void, Error> {
        line.clear();
        bool got_cr = false;
        for (;;) {
            auto b = buffered.read_byte();
            if (!b) return std::unexpected(b.error());
            if (*b == '\n' && got_cr) break;
            if (got_cr) {
                line += '\r';
                got_cr = false;
            }
            if (*b == '\r') { got_cr = true; continue; }
            line += static_cast<char>(*b);
        }
        return {};
    };

    // Main chunked decoding loop.
    for (;;) {
        std::string line;
        auto r = read_line(line);
        if (!r) return std::unexpected(r.error());

        // Parse hex chunk size (ignore chunk-ext after ';').
        const auto semi = line.find(';');
        const auto size_str = (semi != std::string::npos) ? line.substr(0, semi) : line;
        size_t chunk_size = 0;
        try {
            chunk_size = std::stoul(size_str, nullptr, 16);
        } catch (const std::exception &) {
            return std::unexpected(Error::CHUNK_PARSE_FAILED);
        }

        // Zero-length chunk marks the end.
        if (chunk_size == 0) {
            break;
        }

        // Enforce maximum body size.
        if (body_size + chunk_size > max_body_size) {
            return std::unexpected(Error::BODY_TOO_LARGE);
        }

        // Read chunk data.
        body.resize(body_size + chunk_size);
        auto cr = buffered.read_exact(std::span(body.data() + body_size, chunk_size));
        if (!cr) return std::unexpected(cr.error());
        body_size += chunk_size;

        // Read trailing CRLF.
        std::uint8_t crlf[2];
        auto cr2 = buffered.read_exact(crlf);
        if (!cr2) return std::unexpected(cr2.error());
    }

    // Read the trailing CRLF after the last chunk (empty trailer).
    std::uint8_t crlf[2];
    auto cr = buffered.read_exact(crlf);
    if (!cr) return std::unexpected(cr.error());

    body.resize(body_size);
    return body;
}

} // namespace Http
