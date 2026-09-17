//
// Unit tests for DigitalOceanDriver (driver/digital_ocean/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/version/author/capabilities).
//   - update builds the correct DigitalOcean API URL, method, auth header, body.
//   - update with missing config fields returns INVALID_CONFIG.
//   - update succeeds for domain_record responses.
//   - update returns UPSTREAM_REJECTED for error / unparseable responses.
// =============================================================================

#include <gtest/gtest.h>

#include "abi_test_harness.h"

// ── Shared fixtures ──────────────────────────────────────────────────────────

namespace {
    constexpr std::string_view CONFIG = R"({"record_id": "123456", "token": "my-token"})";
} // namespace

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(DigitalOceanDriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor *descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "digital_ocean");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Updates DNS records via the DigitalOcean API");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "2.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(DigitalOceanDriverTest, Update_BasicARecord) {
    FakeHostServices fake;
    fake.queue_response(200, R"({
        "domain_record": {
            "id": 123456,
            "type": "A",
            "name": "www.example.com",
            "data": "1.2.3.4",
            "ttl": 300
        }
    })");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto &request = fake.requests[0];

    // Check URL
    EXPECT_EQ(request.url, "https://api.digitalocean.com/v2/domains/example.com/records/123456");

    // Check method and content type
    EXPECT_EQ(request.method, YADDNSC_HTTP_PUT);
    EXPECT_EQ(request.content_type, "application/json");

    // Check auth header
    const auto auth = request.header("Authorization");
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(*auth, "Bearer my-token");

    // Check request body contains expected fields
    ASSERT_TRUE(request.body.has_value());
    const auto &body = *request.body;
    EXPECT_TRUE(body.find(R"("data":"1.2.3.4")") != std::string::npos);
}

TEST(DigitalOceanDriverTest, Update_SuccessWithAllFields_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, R"({
        "domain_record": {
            "id": 123456,
            "type": "AAAA",
            "name": "www.example.com",
            "data": "::1",
            "ttl": 120,
            "priority": 10,
            "port": 8080,
            "weight": 5,
            "flags": 0,
            "tag": null
        }
    })");

    const auto result = run_abi_update(fake, CONFIG, "::1", "AAAA", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(DigitalOceanDriverTest, Update_MissingRecordId_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"token": "my-token"})", "1.2.3.4", "A", "example.com", "@",
                                       "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(DigitalOceanDriverTest, Update_MissingToken_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"record_id": "123"})", "1.2.3.4", "A", "example.com", "@",
                                       "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(DigitalOceanDriverTest, Update_ErrorResponse_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(404, R"({
        "id": "not_found",
        "message": "The resource you were accessing could not be found."
    })");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(DigitalOceanDriverTest, Update_UnparseableBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "not-json");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(DigitalOceanDriverTest, Update_EmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(DigitalOceanDriverTest, Update_UnknownShape_ReturnsUpstreamRejected) {
    // Body that is valid JSON but doesn't match any known shape.
    FakeHostServices fake;
    fake.queue_response(200, R"({"unknown_field": "value"})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(DigitalOceanDriverTest, Update_TransportError_PropagatesStatus) {
    FakeHostServices fake;
    fake.queue_error(YADDNSC_STATUS_NETWORK_ERROR, "connection refused");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(result.error_message, "connection refused");
}

TEST(DigitalOceanDriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr); // must be a no-op, must not crash
}
