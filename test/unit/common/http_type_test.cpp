//
// Created by Kotarou on 2026/7/7.
//
// Unit tests for http_type.h — HTTP data types.
//
// Verifies:
//   - HttpMethod enumerator values.
//   - HttpRequest aggregate initialisation.
//   - HttpResponse aggregate initialisation.
//   - HttpResult type alias (std::expected).
// =============================================================================

#include <type_traits>

#include <gtest/gtest.h>

#include "http_type.h"

// ── HttpMethod ───────────────────────────────────────────────────────────────

TEST(HttpMethodTest, EnumeratorValues_Defined) {
    EXPECT_EQ(static_cast<int>(HttpMethod::GET), 0);
    EXPECT_EQ(static_cast<int>(HttpMethod::POST), 1);
    EXPECT_EQ(static_cast<int>(HttpMethod::PUT), 2);
    EXPECT_EQ(static_cast<int>(HttpMethod::DEL), 3);
    EXPECT_EQ(static_cast<int>(HttpMethod::PATCH), 4);
    EXPECT_EQ(static_cast<int>(HttpMethod::HEAD), 5);
    EXPECT_EQ(static_cast<int>(HttpMethod::OPTIONS), 6);
}

TEST(HttpMethodTest, IsEnumClass) {
    EXPECT_TRUE((std::is_enum_v<HttpMethod>));
    EXPECT_FALSE((std::is_convertible_v<HttpMethod, int>));
}

// ── HttpParams ───────────────────────────────────────────────────────────────

TEST(HttpParamsTest, DefaultIsEmpty) {
    HttpParams params;
    EXPECT_TRUE(params.empty());
}

TEST(HttpParamsTest, CanInsertKeyValues) {
    HttpParams params;
    params.emplace("Content-Type", "application/json");
    params.emplace("Authorization", "Bearer xyz");
    EXPECT_EQ(params.size(), 2U);
    EXPECT_EQ(params.count("Content-Type"), 1U);
    EXPECT_EQ(params.find("Authorization")->second, "Bearer xyz");
}

TEST(HttpParamsTest, SupportsMultipleValues) {
    HttpParams params;
    params.emplace("Accept", "application/json");
    params.emplace("Accept", "text/plain");
    EXPECT_EQ(params.count("Accept"), 2U);
}

// ── HttpRequest ──────────────────────────────────────────────────────────────

TEST(HttpRequestTest, DefaultGet_NoBody) {
    // Value-initialisation zeroes method; default-init leaves it indeterminate.
    HttpRequest req{};
    EXPECT_FALSE(req.body.has_value());
    EXPECT_TRUE(req.content_type.empty());
    EXPECT_EQ(req.method, HttpMethod::GET);
    EXPECT_TRUE(req.headers.empty());
}

TEST(HttpRequestTest, AggregateInit_PostWithBody) {
    HttpRequest req{
        .content_type = "application/json",
        .method = HttpMethod::POST,
        .headers = {{"X-Custom", "val"}},
        .body = R"({"key":"value"})",
    };
    EXPECT_EQ(req.content_type, "application/json");
    EXPECT_EQ(req.method, HttpMethod::POST);
    EXPECT_EQ(req.headers.size(), 1U);
    ASSERT_TRUE(req.body.has_value());
    EXPECT_EQ(*req.body, R"({"key":"value"})");
}

TEST(HttpRequestTest, IsRelocatable) {
    EXPECT_TRUE(std::is_move_constructible_v<HttpRequest>);
    EXPECT_TRUE(std::is_move_assignable_v<HttpRequest>);
}

// ── HttpResponse ─────────────────────────────────────────────────────────────

TEST(HttpResponseTest, DefaultConstruct) {
    // Value-initialisation zeroes status_code; default-init leaves it indeterminate.
    HttpResponse resp{};
    EXPECT_EQ(resp.status_code, 0);
    EXPECT_TRUE(resp.body.empty());
    EXPECT_TRUE(resp.headers.empty());
}

TEST(HttpResponseTest, AggregateInit) {
    HttpResponse resp{
        .status_code = 200,
        .body = "OK",
        .headers = {{"Content-Length", "2"}},
    };
    EXPECT_EQ(resp.status_code, 200);
    EXPECT_EQ(resp.body, "OK");
    EXPECT_EQ(resp.headers.count("Content-Length"), 1U);
}

TEST(HttpResponseTest, SupportsNonOkStatus) {
    HttpResponse resp{.status_code = 404, .body = "Not Found", .headers = {}};
    EXPECT_EQ(resp.status_code, 404);
}

// ── HttpResult ───────────────────────────────────────────────────────────────

TEST(HttpResultTest, Success_ContainsResponse) {
    HttpResponse resp{.status_code = 200, .body = "OK", .headers = {}};
    HttpResult result(std::move(resp));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->status_code, 200);
}

TEST(HttpResultTest, Error_ContainsMessage) {
    HttpResult result = std::unexpected<std::string>("connection timeout");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "connection timeout");
}

TEST(HttpResultTest, Error_IsMoveConstructible) {
    EXPECT_TRUE(std::is_move_constructible_v<HttpResult>);
}
