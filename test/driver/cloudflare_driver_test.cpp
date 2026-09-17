//
// Unit tests for CloudflareDriver (driver/cloudflare/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/version/author/capabilities).
//   - update builds the correct Cloudflare API URL, method, auth header, body.
//   - update uses config values for zone/record IDs.
//   - update with missing config fields returns INVALID_CONFIG.
//   - update succeeds for success=true responses.
//   - update returns UPSTREAM_REJECTED for success=false / unparseable responses.
// =============================================================================

#include <gtest/gtest.h>

#include "abi_test_harness.h"

// ── Shared fixtures ──────────────────────────────────────────────────────────

namespace {
    constexpr std::string_view CONFIG = R"({
        "zone_id": "myzone",
        "record_id": "rec123",
        "token": "mytoken"
    })";

    std::string make_success_response(std::string_view type, std::string_view name, std::string_view content, int ttl,
                                      bool proxied) {
        return std::string{R"({"success":true,"errors":[],"messages":[],"result":{"id":"rec123","name":")"} +
                           std::string{name} + R"(","type":")" + std::string{type} + R"(","content":")" +
                           std::string{content} + R"(","ttl":)" + std::to_string(ttl) +
                           (proxied ? R"(,"proxied":true)" : R"(,"proxied":false)") + R"(,"proxiable":false}})";
    }
} // namespace

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(CloudflareDriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor *descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "cloudflare");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Updates DNS records via the Cloudflare API");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "2.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(CloudflareDriverTest, Update_BasicARecord) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_response("A", "www.example.com", "1.2.3.4", 30, false));

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto &request = fake.requests[0];

    // Check URL
    EXPECT_EQ(request.url, "https://api.cloudflare.com/client/v4/zones/myzone/dns_records/rec123");

    // Check method and content type
    EXPECT_EQ(request.method, YADDNSC_HTTP_PUT);
    EXPECT_EQ(request.content_type, "application/json");

    // Check auth header
    const auto auth = request.header("Authorization");
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(*auth, "Bearer mytoken");

    // Check request body contains expected fields
    ASSERT_TRUE(request.body.has_value());
    const auto &body = *request.body;
    EXPECT_TRUE(body.find(R"("type":"A")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("content":"1.2.3.4")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("name":"www")") != std::string::npos);
}

TEST(CloudflareDriverTest, Update_WithTtlAndProxied) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_response("AAAA", "example.com", "10.0.0.1", 120, true));

    const auto result = run_abi_update(fake,
                                       R"({"zone_id":"z1","record_id":"r1","token":"t1","ttl":120,"proxied":true})",
                                       "10.0.0.1", "AAAA", "example.com", "@", "example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    const auto &body = *fake.requests[0].body;
    EXPECT_TRUE(body.find(R"("type":"AAAA")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("ttl":120)") != std::string::npos);
    EXPECT_TRUE(body.find(R"("proxied":true)") != std::string::npos);
    EXPECT_TRUE(body.find(R"("content":"10.0.0.1")") != std::string::npos);
}

TEST(CloudflareDriverTest, Update_MissingZoneId_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"record_id":"r1","token":"t1"})", "1.2.3.4", "A", "example.com",
                                       "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(CloudflareDriverTest, Update_MissingToken_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"zone_id":"z1","record_id":"r1"})", "1.2.3.4", "A", "example.com",
                                       "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(CloudflareDriverTest, Update_SuccessWithoutResult_ReturnsOk) {
    // Cloudflare can return success=true with no result field for certain operations.
    FakeHostServices fake;
    fake.queue_response(200, R"({"success":true,"errors":[],"messages":[]})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(CloudflareDriverTest, Update_ErrorWithSource_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({
        "success": false,
        "errors": [{"code": 7003, "message": "Could not find zone", "source": {"pointer": "/zone_id"}}],
        "messages": []
    })");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(CloudflareDriverTest, Update_ErrorWithoutSource_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({
        "success": false,
        "errors": [{"code": 9003, "message": "Record not found"}],
        "messages": []
    })");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(CloudflareDriverTest, Update_UnparseableBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "not-json-at-all");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(CloudflareDriverTest, Update_EmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(CloudflareDriverTest, Update_MultipleErrors_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({
        "success": false,
        "errors": [
            {"code": 1001, "message": "First error"},
            {"code": 1002, "message": "Second error"}
        ],
        "messages": []
    })");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(CloudflareDriverTest, Update_TransportError_PropagatesStatus) {
    FakeHostServices fake;
    fake.queue_error(YADDNSC_STATUS_NETWORK_ERROR, "connection refused");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(result.error_message, "connection refused");
}

TEST(CloudflareDriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr); // must be a no-op, must not crash
}
