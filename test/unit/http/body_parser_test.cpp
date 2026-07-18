//
// Unit tests for src/http/body_parser.cpp — HTTP response body reading.
//
// Tests both Content-Length and Transfer-Encoding: chunked body decoding
// through a pure-memory BufferStream (no actual I/O).
// =============================================================================

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fmt.hpp"
#include <gtest/gtest.h>

#include "http/body_parser.h"
#include "http/header_parser.h"
#include "http/types.h"
#include "network/transport/stream.h"
#include "util/cancellation_token.hpp"

namespace {

// =============================================================================
//  BufferStream — a Transport::Stream that serves data from memory.
//
//  Allows precise control over how much data is available on each read,
//  simulating partial reads, empty streams, and exact boundaries.
// =============================================================================

class BufferStream final : public Transport::Stream {
public:
    explicit BufferStream(std::vector<std::uint8_t> data) noexcept
        : data_(std::move(data)), pos_(0) {}

    BufferStream(std::string_view sv) : data_(sv.begin(), sv.end()), pos_(0) {}

    void set_read_exact_error(Transport::IoError err) noexcept {
        read_exact_error_ = err;
    }

    [[nodiscard]] std::expected<size_t, Transport::IoError> read_some(
        std::span<std::uint8_t> buf,
        const Utils::CancellationToken & /*cancel_token*/) override {
        const size_t avail = data_.size() - pos_;
        const size_t take = std::min(buf.size(), avail);
        if (take == 0) return size_t{0};
        std::memcpy(buf.data(), data_.data() + pos_, take);
        pos_ += take;
        return take;
    }

    [[nodiscard]] std::expected<void, Transport::IoError> read_exact(
        std::span<std::uint8_t> buf,
        const Utils::CancellationToken & /*cancel_token*/) override {
        if (read_exact_error_.has_value()) {
            return std::unexpected(*read_exact_error_);
        }
        const size_t avail = data_.size() - pos_;
        if (avail < buf.size()) {
            return std::unexpected(Transport::IoError::CONNECTION_FAILED);
        }
        std::memcpy(buf.data(), data_.data() + pos_, buf.size());
        pos_ += buf.size();
        return {};
    }

    [[nodiscard]] std::expected<void, Transport::IoError> send_all(
        std::span<const std::uint8_t> /*data*/,
        const Utils::CancellationToken & /*cancel_token*/) override {
        // Not needed for body_parser tests — body_parser only reads.
        return {};
    }

private:
    std::vector<std::uint8_t> data_;
    size_t pos_;
    std::optional<Transport::IoError> read_exact_error_;
};

// =============================================================================
//  Helper — build an HTTP response in a buffer and parse its headers,
//  returning the header info + the raw buffer for body_parser tests.
// =============================================================================

struct ParsedResponse {
    Http::ResponseHeaders headers;
    std::string raw;
};

[[nodiscard]] ParsedResponse make_parsed(std::string raw) {
    // Parse headers — body_parser needs the ResponseHeaders + raw buffer.
    auto r = Http::parse_response(raw, "application/dns-message", 65536);
    if (!r) {
        throw std::runtime_error("Failed to parse test response headers");
    }
    return {.headers = *r, .raw = std::move(raw)};
}

[[nodiscard]] std::string make_headers(int status, std::string_view extra) {
    return fmt::format(
        "HTTP/1.1 {} OK\r\n"
        "Content-Type: application/dns-message\r\n"
        "{}\r\n",
        status, extra);
}

} // anonymous namespace

// =============================================================================
//  Content-Length body reading
// =============================================================================

TEST(HttpBodyParserTest, ContentLength_ExactBody) {
    std::string body_data(32, '\xAB');
    auto p = make_parsed(make_headers(200, fmt::format("Content-Length: {}\r\n", body_data.size()))
                         + body_data);

    BufferStream stream(std::vector<std::uint8_t>{});
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(p.raw.data(), p.raw.size()),
                                  65536, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), body_data.size());
    EXPECT_EQ(*result, std::vector<std::uint8_t>(32, '\xAB'));
}

TEST(HttpBodyParserTest, ContentLength_PartialBuffered) {
    // Only 16 of 32 body bytes are in the initial buffer.
    std::string body_data(32, '\xCD');
    auto raw = make_headers(200, "Content-Length: 32\r\n")
               + body_data.substr(0, 16);
    auto p = make_parsed(raw);

    // Remaining 16 bytes come from the transport.
    auto tail = std::vector<std::uint8_t>(body_data.begin() + 16, body_data.end());
    BufferStream stream(std::move(tail));
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(raw.data(), raw.size()),
                                  65536, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 32u);
    EXPECT_EQ(*result, std::vector<std::uint8_t>(32, '\xCD'));
}

TEST(HttpBodyParserTest, ContentLength_ZeroLength) {
    auto p = make_parsed(make_headers(200, "Content-Length: 0\r\n"));

    BufferStream stream(std::vector<std::uint8_t>{});
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(p.raw.data(), p.raw.size()),
                                  65536, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST(HttpBodyParserTest, ContentLength_AllBuffered) {
    // All body data is in the header buffer — no transport read needed.
    std::string body_data(16, '\xEF');
    auto raw = make_headers(200, "Content-Length: 16\r\n") + body_data;
    auto p = make_parsed(raw);

    BufferStream stream(std::vector<std::uint8_t>{});
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(raw.data(), raw.size()),
                                  65536, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 16u);
}

// =============================================================================
//  Chunked transfer-encoding
// =============================================================================

TEST(HttpBodyParserTest, Chunked_SingleChunk) {
    std::string chunk_data(24, '\x01');
    auto raw = make_headers(200, "Transfer-Encoding: chunked\r\n")
               + fmt::format("{:x}\r\n", chunk_data.size()) + chunk_data + "\r\n"
               + "0\r\n\r\n";
    auto p = make_parsed(raw);

    // Chunked read_body needs data to come from the stream portion after
    // headers, but with chunked encoding the body data is all after headers.
    // The "body_buffered" (raw.size() - header_end) includes the chunked
    // framing, not the raw data, so the BufferedStream will consume from
    // the buffer first, then fall through to the transport.
    BufferStream stream(std::vector<std::uint8_t>{});
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(raw.data(), raw.size()),
                                  65536, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), chunk_data.size());
    EXPECT_EQ(*result, std::vector<std::uint8_t>(24, '\x01'));
}

TEST(HttpBodyParserTest, Chunked_MultipleChunks) {
    std::string chunk1(16, '\x02');
    std::string chunk2(32, '\x03');
    auto raw = make_headers(200, "Transfer-Encoding: chunked\r\n")
               + fmt::format("{:x}\r\n", chunk1.size()) + chunk1 + "\r\n"
               + fmt::format("{:x}\r\n", chunk2.size()) + chunk2 + "\r\n"
               + "0\r\n\r\n";
    auto p = make_parsed(raw);

    BufferStream stream(std::vector<std::uint8_t>{});
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(raw.data(), raw.size()),
                                  65536, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 48u);

    // First 16 bytes = chunk1, next 32 = chunk2.
    EXPECT_EQ(std::vector<std::uint8_t>(result->begin(), result->begin() + 16),
              std::vector<std::uint8_t>(16, '\x02'));
    EXPECT_EQ(std::vector<std::uint8_t>(result->begin() + 16, result->end()),
              std::vector<std::uint8_t>(32, '\x03'));
}

TEST(HttpBodyParserTest, Chunked_ZeroLength) {
    // Only the terminating chunk.
    auto raw = make_headers(200, "Transfer-Encoding: chunked\r\n")
               + "0\r\n\r\n";
    auto p = make_parsed(raw);

    BufferStream stream(std::vector<std::uint8_t>{});
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(raw.data(), raw.size()),
                                  65536, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->empty());
}

TEST(HttpBodyParserTest, Chunked_WithChunkExtensions) {
    std::string chunk_data(8, '\x04');
    auto raw = make_headers(200, "Transfer-Encoding: chunked\r\n")
               + fmt::format("{:x};ext=foobar\r\n", chunk_data.size()) + chunk_data + "\r\n"
               + "0\r\n\r\n";
    auto p = make_parsed(raw);

    BufferStream stream(std::vector<std::uint8_t>{});
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(raw.data(), raw.size()),
                                  65536, cancel);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 8u);
}

TEST(HttpBodyParserTest, Chunked_InvalidHexSize) {
    auto raw = make_headers(200, "Transfer-Encoding: chunked\r\n")
               + "ZZZ\r\n";  // invalid hex
    auto p = make_parsed(raw);

    // We need to provide the chunk data via transport because the
    // "ZZZ\r\n" is body data that starts after headers.
    // Actually, with our BufferedStream, the body after header_end will
    // be consumed from the buffer first.
    BufferStream stream(std::vector<std::uint8_t>{});
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(raw.data(), raw.size()),
                                  65536, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::CHUNK_PARSE_FAILED);
}

// =============================================================================
//  Error conditions
// =============================================================================

TEST(HttpBodyParserTest, ContentLength_TransportFailure) {
    // Body needs more data than the transport provides.
    auto raw = make_headers(200, "Content-Length: 64\r\n") + std::string(16, '\x05');
    auto p = make_parsed(raw);

    BufferStream stream(std::vector<std::uint8_t>(8, '\x06'));  // too short
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(raw.data(), raw.size()),
                                  65536, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::CONNECTION_FAILED);
}

TEST(HttpBodyParserTest, ContentLength_TransportCancelled) {
    // Body needs more data than the transport provides; return CANCELLED.
    auto raw = make_headers(200, "Content-Length: 64\r\n") + std::string(16, '\x05');
    auto p = make_parsed(raw);

    BufferStream stream(std::vector<std::uint8_t>(8, '\x06'));  // too short
    stream.set_read_exact_error(Transport::IoError::CANCELLED);
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(raw.data(), raw.size()),
                                  65536, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::CANCELLED);
}

TEST(HttpBodyParserTest, Chunked_BodyTooLarge) {
    // Each chunk is small, but cumulative exceeds max.
    auto raw = make_headers(200, "Transfer-Encoding: chunked\r\n")
               + "ff\r\n" + std::string(255, '\x08') + "\r\n"
               + "ff\r\n" + std::string(255, '\x08') + "\r\n"
               + "0\r\n\r\n";
    auto p = make_parsed(raw);

    BufferStream stream(std::vector<std::uint8_t>{});
    Utils::CancellationToken cancel;

    auto result = Http::read_body(stream, p.headers,
                                  std::span(raw.data(), raw.size()),
                                  300, cancel);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::BODY_TOO_LARGE);
}
