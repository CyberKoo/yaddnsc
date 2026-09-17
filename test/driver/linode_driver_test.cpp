//
// Unit tests for LinodeDriver (driver/linode/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/version/author/capabilities).
//   - update builds the correct Linode API URL, method, auth header, body.
//   - update includes ttl_sec when configured.
//   - update with missing config fields returns INVALID_CONFIG.
//   - update succeeds for HTTP 200 responses.
//   - update returns UPSTREAM_REJECTED for non-200 error/empty bodies.
// =============================================================================

#include <gtest/gtest.h>

#include "abi_test_harness.h"

// ── Shared fixtures ──────────────────────────────────────────────────────────

namespace {
    constexpr std::string_view CONFIG = R"({
        "token": "my-token",
        "domain_id": "dom123",
        "record_id": "rec456"
    })";

    const std::string SUCCESS_BODY = R"({"id": 123, "type": "A", "name": "www", "target": "1.2.3.4", "ttl_sec": 300})";
} // namespace

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(LinodeDriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor *descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "linode");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Updates DNS records via the Linode API");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "1.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(LinodeDriverTest, Update_BasicARecord) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_BODY);

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto &request = fake.requests[0];

    // Check URL
    EXPECT_EQ(request.url, "https://api.linode.com/v4/domains/dom123/records/rec456");

    // Check method and content type
    EXPECT_EQ(request.method, YADDNSC_HTTP_PUT);
    EXPECT_EQ(request.content_type, "application/json");

    // Check auth header
    const auto auth = request.header("Authorization");
    ASSERT_TRUE(auth.has_value());
    EXPECT_EQ(*auth, "Bearer my-token");

    // Check body
    ASSERT_TRUE(request.body.has_value());
    const auto &body = *request.body;
    EXPECT_TRUE(body.find(R"("name":"www")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("target":"1.2.3.4")") != std::string::npos);
    // ttl_sec is optional and should be omitted when not set
    EXPECT_TRUE(body.find("ttl_sec") == std::string::npos);
}

TEST(LinodeDriverTest, Update_WithTtlSec) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_BODY);

    const auto result = run_abi_update(fake,
                                       R"({"token": "t1", "domain_id": "d1", "record_id": "r1", "ttl_sec": 3600})",
                                       "10.0.0.1", "AAAA", "example.com", "@", "example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    EXPECT_TRUE(fake.requests[0].body->find(R"("ttl_sec":3600)") != std::string::npos);
}

TEST(LinodeDriverTest, Update_MissingToken_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"domain_id": "d1", "record_id": "r1"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(LinodeDriverTest, Update_MissingDomainId_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"token": "t1", "record_id": "r1"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(LinodeDriverTest, Update_200_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_BODY);
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(LinodeDriverTest, Update_200_EmptyBody_ReturnsOk) {
    // Linode returns 200 on success even with minimal body;
    // our implementation checks HTTP 200 first.
    FakeHostServices fake;
    fake.queue_response(200, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(LinodeDriverTest, Update_Non200_WithErrorBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({"errors": [{"field": "type", "reason": "Invalid record type"}]})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(LinodeDriverTest, Update_Non200_WithMultipleErrors_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({
        "errors": [
            {"field": "name", "reason": "Invalid name"},
            {"field": "target", "reason": "Invalid target"}
        ]
    })");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(LinodeDriverTest, Update_Non200_WithEmptyFieldError_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({"errors": [{"field": "", "reason": "Rate limit exceeded"}]})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(LinodeDriverTest, Update_Non200_UnparseableBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, "not-json");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(LinodeDriverTest, Update_Non200_EmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(500, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(LinodeDriverTest, Update_Non200_ErrorFieldEmpty_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({"errors": [{"field": "type", "reason": "Invalid"}]})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(LinodeDriverTest, Update_Non200_NoErrorsKey_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({"other": "data"})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(LinodeDriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr); // must be a no-op, must not crash
}
