//
// Unit tests for src/http/header_parser.cpp — HTTP response parsing.
//
// Verifies:
//   - Valid response → returns ResponseHeaders with correct fields.
//   - Incomplete data → Error::INCOMPLETE.
//   - Malformed response → Error::HEADER_PARSE_FAILED.
//   - Content-Type mismatch → CONTENT_TYPE_MISMATCH.
//   - Body too large → BODY_TOO_LARGE.
//   - Chunked encoding → is_chunked=true.
//   - No Content-Length → has_content_length=false.
//   - Multiple headers of the same type → first wins.
// =============================================================================

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "fmt.hpp"
#include "http/header_parser.h"

// ── Helpers ─────────────────────────────────────────────────────────────────

/// Build a complete HTTP response string.
[[nodiscard]] std::string make_response(int status_code, std::string_view content_type,
                                        std::string_view body,
                                        std::string_view extra_headers = "") {
    std::string resp;
    resp += fmt::format("HTTP/1.1 {} OK\r\n", status_code);
    resp += fmt::format("Content-Length: {}\r\n", body.size());
    if (!content_type.empty()) {
        resp += fmt::format("Content-Type: {}\r\n", content_type);
    }
    if (!extra_headers.empty()) {
        resp += extra_headers;
        if (!extra_headers.ends_with("\r\n")) resp += "\r\n";
    }
    resp += "\r\n";
    resp += body;
    return resp;
}

[[nodiscard]] std::string make_chunked_response(int status_code, std::string_view content_type,
                                                 std::string_view chunk_data) {
    std::string resp;
    resp += fmt::format("HTTP/1.1 {} OK\r\n", status_code);
    resp += "Transfer-Encoding: chunked\r\n";
    if (!content_type.empty()) {
        resp += fmt::format("Content-Type: {}\r\n", content_type);
    }
    resp += "\r\n";
    resp += fmt::format("{:x}\r\n{}\r\n", chunk_data.size(), chunk_data);
    resp += "0\r\n\r\n";
    return resp;
}

// =============================================================================
//  Valid responses
// =============================================================================

TEST(HttpHeaderParserTest, Simple200Ok_WithMatchingContentType) {
    auto resp = make_response(200, "application/json", R"({"key":"value"})");

    auto result = Http::parse_response(resp, "application/json", 1024);
    ASSERT_TRUE(result.has_value());

    EXPECT_EQ(result->status_code, 200);
    EXPECT_EQ(result->content_length, 15u);
    EXPECT_TRUE(result->has_content_length);
    EXPECT_FALSE(result->is_chunked);
}

TEST(HttpHeaderParserTest, StatusCodePreserved) {
    auto resp = make_response(404, "text/plain", "Not Found");

    auto result = Http::parse_response(resp, "text/plain", 1024);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 404);
}

TEST(HttpHeaderParserTest, HeaderEndOffset_IsCorrect) {
    std::string_view body = "hello world";
    auto resp = make_response(200, "text/plain", body);

    auto result = Http::parse_response(resp, "text/plain", 1024);
    ASSERT_TRUE(result.has_value());

    // header_end should point to the start of the body.
    EXPECT_EQ(resp.substr(result->header_end), body);
}

// ── Content-Type matching ──────────────────────────────────────────────────

TEST(HttpHeaderParserTest, ContentTypeSubstringMatch) {
    // "application/json" contains "json" — should match when expected is "json".
    auto resp = make_response(200, "application/json; charset=utf-8", R"({})");
    auto result = Http::parse_response(resp, "json", 1024);
    ASSERT_TRUE(result.has_value());
}

TEST(HttpHeaderParserTest, ContentTypeCaseInsensitive) {
    auto resp = make_response(200, "APPLICATION/JSON", R"({})");
    auto result = Http::parse_response(resp, "application/json", 1024);
    ASSERT_TRUE(result.has_value());
}

// ── Error conditions ───────────────────────────────────────────────────────

TEST(HttpHeaderParserTest, IncompleteResponse_ReturnsIncomplete) {
    // Only partial header data.
    std::string_view buf = "HTTP/1.1 200 OK\r\nCon";

    auto result = Http::parse_response(buf, "application/json", 1024);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::INCOMPLETE);
}

TEST(HttpHeaderParserTest, EmptyBuffer_ReturnsIncomplete) {
    auto result = Http::parse_response("", "application/json", 1024);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::INCOMPLETE);
}

TEST(HttpHeaderParserTest, MalformedResponse_ReturnsParseFailed) {
    std::string_view buf = "NOT AN HTTP RESPONSE\r\n\r\n";

    auto result = Http::parse_response(buf, "application/json", 1024);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::HEADER_PARSE_FAILED);
}

TEST(HttpHeaderParserTest, ContentTypeMismatch_ReturnsError) {
    auto resp = make_response(200, "text/html", "<html></html>");

    auto result = Http::parse_response(resp, "application/json", 1024);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::CONTENT_TYPE_MISMATCH);
}

TEST(HttpHeaderParserTest, MissingContentType_ReturnsError) {
    auto resp = make_response(200, "", "body");

    auto result = Http::parse_response(resp, "application/json", 1024);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::CONTENT_TYPE_MISMATCH);
}

TEST(HttpHeaderParserTest, BodyExceedsMaxSize_ReturnsBodyTooLarge) {
    // Content-Length > max_body_size.
    std::string body(2000, 'x');
    auto resp = make_response(200, "text/plain", body);

    auto result = Http::parse_response(resp, "text/plain", 1024);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::BODY_TOO_LARGE);
}

TEST(HttpHeaderParserTest, BodySizeExactlyAtLimit_Succeeds) {
    std::string body(1024, 'x');
    auto resp = make_response(200, "text/plain", body);

    auto result = Http::parse_response(resp, "text/plain", 1024);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content_length, 1024u);
}

// ── Chunked encoding ───────────────────────────────────────────────────────

TEST(HttpHeaderParserTest, ChunkedEncoding_Detected) {
    auto resp = make_chunked_response(200, "text/plain", "chunk data");

    auto result = Http::parse_response(resp, "text/plain", 1024);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_chunked);
}

TEST(HttpHeaderParserTest, ChunkedEncoding_NoContentLength) {
    auto resp = make_chunked_response(200, "text/plain", "data");

    auto result = Http::parse_response(resp, "text/plain", 1024);
    ASSERT_TRUE(result.has_value());
    // Chunked responses typically don't have Content-Length.
    // has_content_length should be false because we didn't set it.
    EXPECT_FALSE(result->has_content_length);
}

// ── Edge cases ─────────────────────────────────────────────────────────────

TEST(HttpHeaderParserTest, InvalidContentLength_ReturnsParseFailed) {
    // Content-Length with non-numeric value.
    std::string resp = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/plain\r\n"
                       "Content-Length: abc\r\n"
                       "\r\n";

    auto result = Http::parse_response(resp, "text/plain", 1024);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), Http::Error::HEADER_PARSE_FAILED);
}

TEST(HttpHeaderParserTest, ContentLengthZero_Succeeds) {
    auto resp = make_response(204, "application/json", "");

    auto result = Http::parse_response(resp, "application/json", 1024);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content_length, 0u);
    EXPECT_TRUE(result->has_content_length);
}

TEST(HttpHeaderParserTest, ResponseWithoutContentLength_NoBody) {
    // HTTP response with no Content-Length and no body (e.g. 204 No Content).
    std::string resp = "HTTP/1.1 204 No Content\r\n"
                       "Content-Type: application/json\r\n"
                       "\r\n";

    auto result = Http::parse_response(resp, "application/json", 1024);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 204);
    EXPECT_FALSE(result->has_content_length);
}

// ── Edge cases ────────────────────────────────────────────────────────────

TEST(HttpHeaderParserTest, EmptyExpectedContentType_SkipsTypeCheck) {
    // When expected_content_type is empty, any content-type (or none) is accepted.
    std::string resp = "HTTP/1.1 200 OK\r\n"
                       "\r\n"
                       "body";

    auto result = Http::parse_response(resp, "", 1024);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 200);
}

TEST(HttpHeaderParserTest, MultipleContentLength_FirstWins) {
    // When multiple Content-Length headers are present, the first one is used.
    std::string resp = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/plain\r\n"
                       "Content-Length: 5\r\n"
                       "Content-Length: 999\r\n"
                       "\r\n"
                       "hello";

    auto result = Http::parse_response(resp, "text/plain", 1024);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content_length, 5u);
    EXPECT_TRUE(result->has_content_length);
}

TEST(HttpHeaderParserTest, TransferEncodingChunkedWithGzip) {
    // Transfer-Encoding: gzip, chunked (comma-separated, chunked last).
    std::string resp = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/plain\r\n"
                       "Transfer-Encoding: gzip, chunked\r\n"
                       "\r\n";

    auto result = Http::parse_response(resp, "text/plain", 1024);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->is_chunked);
    EXPECT_FALSE(result->has_content_length);
}

TEST(HttpHeaderParserTest, ContentLengthWithLeadingZeros) {
    // Content-Length with leading zeros should still parse correctly.
    std::string resp = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/plain\r\n"
                       "Content-Length: 0015\r\n"
                       "\r\n"
                       "hello world!!!";

    auto result = Http::parse_response(resp, "text/plain", 1024);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->content_length, 15u);
    EXPECT_TRUE(result->has_content_length);
}

TEST(HttpHeaderParserTest, ContentTypeWithoutExpected_NoMismatch) {
    // If expected_content_type is empty, any content-type is accepted.
    std::string resp = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/xml\r\n"
                       "Content-Length: 4\r\n"
                       "\r\n"
                       "body";

    auto result = Http::parse_response(resp, "", 1024);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 200);
}
