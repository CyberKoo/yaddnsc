//
// Unit tests for GoDaddyDriver (driver/godaddy/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/version/author/capabilities).
//   - update builds the correct GoDaddy API URL with domain/type/name.
//   - update sets the sso-key Authorization header.
//   - update produces a JSON array body with a single record and configurable TTL.
//   - update with missing config fields returns INVALID_CONFIG.
//   - update succeeds for HTTP 200 (empty or non-empty body).
//   - update returns UPSTREAM_REJECTED for non-200 status codes.
// =============================================================================

#include <gtest/gtest.h>

#include "abi_test_harness.h"

// ── Shared fixtures ──────────────────────────────────────────────────────────

namespace {
    constexpr std::string_view CONFIG = R"({
        "key": "mykey",
        "secret": "mysecret"
    })";
} // namespace

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(GoDaddyDriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor *descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "godaddy");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Updates DNS records via the GoDaddy API");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "1.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(GoDaddyDriverTest, Update_BasicARecord) {
    FakeHostServices fake;
    // GoDaddy returns 200 OK with an empty body on success.
    fake.queue_response(200, "");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto &request = fake.requests[0];

    // Check URL
    EXPECT_EQ(request.url,
              "https://api.godaddy.com/v1/domains/example.com/records/A/www");

    // Check method and content type
    EXPECT_EQ(request.method, YADDNSC_HTTP_PUT);
    EXPECT_EQ(request.content_type, "application/json");

    // Check auth header (sso-key)
    const auto auth = request.header("Authorization");
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(*auth, "sso-key mykey:mysecret");

    // Check body: GoDaddy expects an array with a single record
    ASSERT_TRUE(request.body.has_value());
    const auto &body = *request.body;
    EXPECT_TRUE(body.starts_with("["));
    EXPECT_TRUE(body.ends_with("]"));
    EXPECT_TRUE(body.find(R"("data":"1.2.3.4")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("type":"A")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("ttl":600)") != std::string::npos);
}

TEST(GoDaddyDriverTest, Update_WithCustomTtl) {
    FakeHostServices fake;
    fake.queue_response(200, "");

    const auto result = run_abi_update(fake, R"({"key": "mykey", "secret": "mysecret", "ttl": 1200})",
                                       "10.0.0.1", "AAAA", "example.org", "@", "example.org");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    const auto &body = *fake.requests[0].body;
    EXPECT_TRUE(body.find(R"("ttl":1200)") != std::string::npos);
}

TEST(GoDaddyDriverTest, Update_MissingKey_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"secret": "mysecret"})", "1.2.3.4", "A", "example.com",
                                       "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(GoDaddyDriverTest, Update_MissingSecret_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"key": "mykey"})", "1.2.3.4", "A", "example.com",
                                       "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(GoDaddyDriverTest, Update_SuccessWithBody_ReturnsOk) {
    // GoDaddy only documents an empty body, but any 200 is a success.
    FakeHostServices fake;
    fake.queue_response(200, "some body");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(GoDaddyDriverTest, Update_Non200WithBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({"message":"Bad Request"})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(GoDaddyDriverTest, Update_Non200EmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(500, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(GoDaddyDriverTest, Update_ForbiddenWithBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(403, R"({"message":"Forbidden"})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

// ── validate (OPTIONAL yaddnsc_driver_validate entry) ────────────────────────
//
// Validation is a pure parse of driver_param against the driver's schema: a
// valid config passes; a missing required key and malformed JSON both map to
// YADDNSC_STATUS_INVALID_CONFIG. No HTTP exchange is queued or expected.

TEST(GoDaddyDriverTest, Validate_ValidConfig_Succeeds) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, CONFIG);
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(GoDaddyDriverTest, Validate_MissingSecret_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({"key":"mykey"})");
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(GoDaddyDriverTest, Validate_MalformedJson_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({invalid)");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(fake.requests.empty());
}

TEST(GoDaddyDriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr); // must be a no-op, must not crash
}
