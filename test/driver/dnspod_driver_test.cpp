//
// Unit tests for DNSPodDriver (driver/dnspod/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/version/author/capabilities).
//   - update builds the correct DNSPod API URL (CN / global endpoint).
//   - update produces a form-encoded body with all expected fields.
//   - update uses "默认" record_line by default for CN, "default" for global.
//   - update with missing config fields returns INVALID_CONFIG.
//   - update succeeds for status code "1" responses (with/without record).
//   - update returns UPSTREAM_REJECTED for error status / missing status /
//     unparseable or empty response bodies.
// =============================================================================

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "abi_test_harness.h"

// ── Shared fixtures ──────────────────────────────────────────────────────────

namespace {
constexpr std::string_view CONFIG = R"({
        "domain_id": "dom123",
        "record_id": "rec456",
        "login_token": "token123",
        "record_line_id": "0",
        "global": false
    })";

// Mirrors the real DNSPod Record.Modify response, whose record object
// carries fields the driver does not model (line/ttl/status/...); parsing
// must tolerate them (glaze errors on unknown keys by default).
const std::string SUCCESS_WITH_RECORD = R"({
        "status": {"code": "1", "message": "Action completed successfully", "created_at": "2024-01-01 00:00:00"},
        "record": {"id": 123, "name": "www.example.com", "value": "1.2.3.4",
                   "line": "默认", "line_id": "10=0", "type": "A", "ttl": 600,
                   "weight": null, "mx": 0, "enabled": true, "status": "enable",
                   "monitor_status": "", "remark": "", "updated_on": "2024-01-01 00:00:00"}
    })";

constexpr std::string_view GLOBAL_CONFIG = R"({
        "domain_id": "dom123",
        "record_id": "rec456",
        "login_token": "token123",
        "global": true,
        "record_line_id": "0"
    })";
}  // namespace

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(DNSPodDriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor* descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "dnspod");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Updates DNS records via the DNSPod API");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "2.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(DNSPodDriverTest, Update_DefaultEndpointCn) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_WITH_RECORD);

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto& request = fake.requests[0];

    EXPECT_EQ(request.url, "https://dnsapi.cn/Record.Ddns");
    EXPECT_EQ(request.method, YADDNSC_HTTP_POST);
    EXPECT_EQ(request.content_type, "application/x-www-form-urlencoded");
}

TEST(DNSPodDriverTest, Update_GlobalEndpoint) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_WITH_RECORD);

    const auto result = run_abi_update(fake, GLOBAL_CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    EXPECT_EQ(fake.requests[0].url, "https://api.dnspod.com/Record.Ddns");
}

TEST(DNSPodDriverTest, Update_BodyContainsRequiredFields) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_WITH_RECORD);

    const auto result = run_abi_update(fake, CONFIG, "10.0.0.1", "AAAA", "example.com", "@", "example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    const auto& body = *fake.requests[0].body;

    EXPECT_TRUE(body.find("login_token=token123") != std::string::npos);
    EXPECT_TRUE(body.find("domain_id=dom123") != std::string::npos);
    EXPECT_TRUE(body.find("record_id=rec456") != std::string::npos);
    EXPECT_TRUE(body.find("sub_domain=@") != std::string::npos);
    EXPECT_TRUE(body.find("record_type=AAAA") != std::string::npos);
    EXPECT_TRUE(body.find("value=10.0.0.1") != std::string::npos);
    EXPECT_TRUE(body.find("format=json") != std::string::npos);
}

TEST(DNSPodDriverTest, Update_DefaultRecordLine_Cn) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_WITH_RECORD);

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    // CN default record_line should be "默认"
    const auto& body = *fake.requests[0].body;
    EXPECT_TRUE(body.find("record_line=%E9%BB%98%E8%AE%A4") != std::string::npos ||
                body.find("record_line=默认") != std::string::npos);
}

TEST(DNSPodDriverTest, Update_DefaultRecordLine_Global) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_WITH_RECORD);

    const auto result = run_abi_update(fake, GLOBAL_CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    EXPECT_TRUE(fake.requests[0].body->find("record_line=default") != std::string::npos);
}

TEST(DNSPodDriverTest, Update_CustomRecordLine) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_WITH_RECORD);

    const auto result = run_abi_update(fake,
                                       R"({
                                            "domain_id": "dom123",
                                            "record_id": "rec456",
                                            "login_token": "token123",
                                            "record_line": "unicom",
                                            "record_line_id": "0",
                                            "global": false
                                        })",
                                       "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    EXPECT_TRUE(fake.requests[0].body->find("record_line=unicom") != std::string::npos);
}

TEST(DNSPodDriverTest, Update_MissingDomainId_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"record_id": "rec456", "login_token": "token123"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(DNSPodDriverTest, Update_MissingLoginToken_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"domain_id": "dom123", "record_id": "rec456"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(DNSPodDriverTest, Update_StatusCode1WithRecord_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, R"({
        "status": {"code": "1", "message": "Action completed successfully", "created_at": "2024-01-01 00:00:00"},
        "record": {"id": 123, "name": "www.example.com", "value": "1.2.3.4"}
    })");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(DNSPodDriverTest, Update_StatusCode1NoRecord_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, R"({
        "status": {"code": "1", "message": "Action completed successfully", "created_at": "2024-01-01 00:00:00"}
    })");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(DNSPodDriverTest, Update_ErrorStatusCode_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, R"({
        "status": {"code": "-1", "message": "Login fails", "created_at": "2024-01-01 00:00:00"}
    })");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(DNSPodDriverTest, Update_MissingStatus_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, R"({"record": {"id": 123, "name": "www", "value": "1.2.3.4"}})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(DNSPodDriverTest, Update_UnparseableBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "not-json");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(DNSPodDriverTest, Update_EmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

// ── validate (OPTIONAL yaddnsc_driver_validate entry) ────────────────────────
//
// Validation is a pure parse of driver_param against the driver's schema: a
// valid config passes; a missing required key and malformed JSON both map to
// YADDNSC_STATUS_INVALID_CONFIG. No HTTP exchange is queued or expected.

TEST(DNSPodDriverTest, Validate_ValidConfig_Succeeds) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, CONFIG);
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(DNSPodDriverTest, Validate_MissingLoginToken_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({"domain_id":"dom123","record_id":"rec456"})");
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(DNSPodDriverTest, Validate_MalformedJson_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({invalid)");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(fake.requests.empty());
}

TEST(DNSPodDriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr);  // must be a no-op, must not crash
}
