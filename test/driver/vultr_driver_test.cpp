//
// Unit tests for VultrDriver (driver/vultr/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/version/author/capabilities).
//   - update builds the correct Vultr API URL with domain/record_id.
//   - update sets Bearer authorization.
//   - update produces JSON body with name/data fields and ttl when configured.
//   - update with missing config fields returns INVALID_CONFIG.
//   - update succeeds for HTTP 204 No Content.
//   - update returns UPSTREAM_REJECTED for non-204 / error bodies.
// =============================================================================

#include <gtest/gtest.h>

#include "abi_test_harness.h"

// ── Shared fixtures ──────────────────────────────────────────────────────────

namespace {
    constexpr std::string_view CONFIG = R"({
        "api_key": "my-key",
        "record_id": "rec123"
    })";
} // namespace

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(VultrDriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor *descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "vultr");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Updates DNS records via the Vultr API");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "1.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(VultrDriverTest, Update_BasicARecord) {
    FakeHostServices fake;
    fake.queue_response(204, "");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto &request = fake.requests[0];

    // Check URL
    EXPECT_EQ(request.url, "https://api.vultr.com/v2/domains/example.com/records/rec123");

    // Check method and content type
    EXPECT_EQ(request.method, YADDNSC_HTTP_PATCH);
    EXPECT_EQ(request.content_type, "application/json");

    // Check auth header
    const auto auth = request.header("Authorization");
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(*auth, "Bearer my-key");

    // Check body
    ASSERT_TRUE(request.body.has_value());
    const auto &body = *request.body;
    EXPECT_TRUE(body.find(R"("name":"www")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("data":"1.2.3.4")") != std::string::npos);
    // ttl should be omitted when not configured
    EXPECT_TRUE(body.find("ttl") == std::string::npos);
}

TEST(VultrDriverTest, Update_WithTtl) {
    FakeHostServices fake;
    fake.queue_response(204, "");

    const auto result = run_abi_update(fake, R"({"api_key":"my-key","record_id":"rec123","ttl":600})", "10.0.0.1",
                                       "AAAA", "example.com", "@", "example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    EXPECT_TRUE(fake.requests[0].body.value().find(R"("ttl":600)") != std::string::npos);
}

TEST(VultrDriverTest, Update_MissingApiKey_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"record_id":"rec123"})", "1.2.3.4", "A", "example.com", "@",
                                       "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(VultrDriverTest, Update_MissingRecordId_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"api_key":"my-key"})", "1.2.3.4", "A", "example.com", "@",
                                       "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(VultrDriverTest, Update_204_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(204, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(VultrDriverTest, Update_Non204_WithErrorBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({"errors":[{"detail":"Invalid record ID"}]})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(VultrDriverTest, Update_Non204_WithMultipleErrors_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({
        "errors": [
            {"detail": "Invalid API key"},
            {"detail": "Rate limit exceeded"}
        ]
    })");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(VultrDriverTest, Update_Non204_UnparseableBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, "not-json");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(VultrDriverTest, Update_Non204_EmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(500, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(VultrDriverTest, Update_Non204_UnexpectedBody_ReturnsUpstreamRejected) {
    // Even with 200 status, Vultr returns 204 on success.
    // But 200 with empty body is not expected — treat as failure.
    FakeHostServices fake;
    fake.queue_response(200, "something");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(VultrDriverTest, Update_Non204_NoRelevantErrorKey_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({"some_other_key": "value"})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

// ── validate (OPTIONAL yaddnsc_driver_validate entry) ────────────────────────
//
// Validation is a pure parse of driver_param against the driver's schema: a
// valid config passes; a missing required key and malformed JSON both map to
// YADDNSC_STATUS_INVALID_CONFIG. No HTTP exchange is queued or expected.

TEST(VultrDriverTest, Validate_ValidConfig_Succeeds) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, CONFIG);
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(VultrDriverTest, Validate_MissingApiKey_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({"record_id":"rec123"})");
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(VultrDriverTest, Validate_MalformedJson_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({invalid)");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(fake.requests.empty());
}

TEST(VultrDriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr); // must be a no-op, must not crash
}
