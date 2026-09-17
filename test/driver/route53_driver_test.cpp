//
// Unit tests for Route53Driver (driver/route53/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/version/author/capabilities).
//   - update builds the correct Route 53 API URL.
//   - update sets required SigV4 headers.
//   - update produces a valid XML request body with UPSERT action.
//   - update uses configurable TTL.
//   - update ensures FQDN has a trailing dot.
//   - update with missing config fields returns INVALID_CONFIG.
//   - update succeeds for XML with PENDING / INSYNC status.
//   - update returns UPSTREAM_REJECTED for non-200, malformed, or empty bodies.
// =============================================================================

#include <format>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "abi_test_harness.h"

// ── Shared fixtures ──────────────────────────────────────────────────────────

namespace {
    constexpr std::string_view CONFIG = R"({
        "access_key_id": "AKID123",
        "secret_access_key": "secret456",
        "hosted_zone_id": "Z3M79L5CQABCDE",
        "region": "us-east-1",
        "record_name": "www.example.com"
    })";

/// Build a Route 53 ChangeResourceRecordSets success response XML.
std::string make_success_xml(std::string_view status) {
    return std::format(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<ChangeResourceRecordSetsResponse xmlns="https://route53.amazonaws.com/doc/2013-04-01/">
  <ChangeInfo>
    <Id>/change/C2682N5HXP0BZ4</Id>
    <Status>{}</Status>
    <SubmittedAt>2024-01-01T00:00:00Z</SubmittedAt>
  </ChangeInfo>
</ChangeResourceRecordSetsResponse>)", status);
}

/// Build a Route 53 error response XML.
std::string make_error_xml(std::string_view code, std::string_view message) {
    return std::format(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<ErrorResponse xmlns="https://route53.amazonaws.com/doc/2013-04-01/">
  <Error>
    <Type>Sender</Type>
    <Code>{}</Code>
    <Message>{}</Message>
  </Error>
  <RequestId>req123</RequestId>
</ErrorResponse>)", code, message);
}
} // namespace

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(Route53DriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor *descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "route53");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Updates DNS records via the AWS Route 53 API");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "1.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(Route53DriverTest, Update_BasicARecord) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_xml("PENDING"));

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto &request = fake.requests[0];

    // Check URL
    EXPECT_EQ(request.url,
              "https://route53.amazonaws.com/2013-04-01/hostedzone/Z3M79L5CQABCDE/rrset");

    // Check method and content type
    EXPECT_EQ(request.method, YADDNSC_HTTP_POST);
    EXPECT_EQ(request.content_type, "application/xml");

    // Check SigV4 headers are present
    const auto host = request.header("Host");
    ASSERT_TRUE(host.has_value());
    EXPECT_EQ(*host, "route53.amazonaws.com");
    EXPECT_TRUE(request.header("X-Amz-Date").has_value());
    EXPECT_TRUE(request.header("X-Amz-Content-SHA256").has_value());

    // Verify Authorization header starts with AWS4-HMAC-SHA256
    const auto auth = request.header("Authorization");
    ASSERT_TRUE(auth.has_value());
    EXPECT_TRUE(auth->starts_with("AWS4-HMAC-SHA256"));
    EXPECT_TRUE(auth->find("Credential=AKID123") != std::string::npos);
    EXPECT_TRUE(auth->find("us-east-1/route53/aws4_request") != std::string::npos);

    // Check body contains XML
    ASSERT_TRUE(request.body.has_value());
    const auto &body = *request.body;
    EXPECT_TRUE(body.find("ChangeResourceRecordSetsRequest") != std::string::npos);
    EXPECT_TRUE(body.find("UPSERT") != std::string::npos);
    EXPECT_TRUE(body.find("www.example.com.") != std::string::npos);  // trailing dot
    EXPECT_TRUE(body.find("A") != std::string::npos);
    EXPECT_TRUE(body.find("1.2.3.4") != std::string::npos);
    EXPECT_TRUE(body.find("<TTL>300</TTL>") != std::string::npos);  // default TTL
}

TEST(Route53DriverTest, Update_FqdnWithoutDot_AddsTrailingDot) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_xml("PENDING"));

    const auto result = run_abi_update(fake,
                                       R"({
        "access_key_id": "AKID123",
        "secret_access_key": "secret456",
        "hosted_zone_id": "ZONE1",
        "region": "us-west-2",
        "record_name": "test.example.com"
    })",
                                       "10.0.0.1", "AAAA", "example.com", "test", "test.example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    // Route 53 requires FQDN with trailing dot
    EXPECT_TRUE(fake.requests[0].body->find("test.example.com.") != std::string::npos);
}

TEST(Route53DriverTest, Update_WithCustomTtl) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_xml("PENDING"));

    const auto result = run_abi_update(fake,
                                       R"({
        "access_key_id": "AKID123",
        "secret_access_key": "secret456",
        "hosted_zone_id": "ZONE1",
        "region": "eu-west-1",
        "record_name": "www.example.com",
        "ttl": 60
    })",
                                       "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    EXPECT_TRUE(fake.requests[0].body->find("<TTL>60</TTL>") != std::string::npos);
}

TEST(Route53DriverTest, Update_MissingAccessKey_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake,
                                       R"({
        "secret_access_key": "secret456",
        "hosted_zone_id": "ZONE1",
        "region": "us-east-1",
        "record_name": "test"
    })",
                                       "1.2.3.4", "A", "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(Route53DriverTest, Update_MissingHostedZoneId_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake,
                                       R"({
        "access_key_id": "AKID123",
        "secret_access_key": "secret456",
        "region": "us-east-1",
        "record_name": "test"
    })",
                                       "1.2.3.4", "A", "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(Route53DriverTest, Update_PendingStatus_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_xml("PENDING"));
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(Route53DriverTest, Update_InsyncStatus_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_xml("INSYNC"));
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(Route53DriverTest, Update_UnexpectedStatus_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_xml("FAILED"));
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(Route53DriverTest, Update_MissingStatus_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, R"(<?xml version="1.0" encoding="UTF-8"?>
<ChangeResourceRecordSetsResponse xmlns="https://route53.amazonaws.com/doc/2013-04-01/">
  <ChangeInfo>
    <Id>/change/C2682N5HXP0BZ4</Id>
    <SubmittedAt>2024-01-01T00:00:00Z</SubmittedAt>
  </ChangeInfo>
</ChangeResourceRecordSetsResponse>)");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(Route53DriverTest, Update_Non200WithErrorXml_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, make_error_xml("InvalidChangeBatch", "RRset with name www.example.com. and type A is not supported"));
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(Route53DriverTest, Update_Non200WithMultipleErrors_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(403, R"(<?xml version="1.0" encoding="UTF-8"?>
<ErrorResponse xmlns="https://route53.amazonaws.com/doc/2013-04-01/">
  <Error>
    <Type>Sender</Type>
    <Code>AccessDenied</Code>
    <Message>User is not authorized</Message>
  </Error>
  <Error>
    <Type>Sender</Type>
    <Code>Throttling</Code>
    <Message>Rate exceeded</Message>
  </Error>
  <RequestId>req456</RequestId>
</ErrorResponse>)");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(Route53DriverTest, Update_Non200UnparseableBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, "not xml");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(Route53DriverTest, Update_Non200EmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(500, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(Route53DriverTest, Update_EmptyFqdn_UsesDot) {
    // ensure_trailing_dot("") returns "." — Route 53 requires a dot.
    FakeHostServices fake;
    fake.queue_response(200, make_success_xml("PENDING"));

    const auto result = run_abi_update(fake,
                                       R"({
        "access_key_id": "AKID123",
        "secret_access_key": "secret456",
        "hosted_zone_id": "ZONE1",
        "region": "us-east-1",
        "record_name": "test"
    })",
                                       "1.2.3.4", "A", "example.com", "", "");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    EXPECT_TRUE(fake.requests[0].body->find(">.<") != std::string::npos);
}

TEST(Route53DriverTest, Update_FqdnWithTrailingDot_NotDuplicated) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_xml("PENDING"));

    const auto result = run_abi_update(fake,
                                       R"({
        "access_key_id": "AKID123",
        "secret_access_key": "secret456",
        "hosted_zone_id": "ZONE1",
        "region": "us-east-1",
        "record_name": "test"
    })",
                                       "1.2.3.4", "A", "example.com", "www", "www.example.com.");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    const auto &body = *fake.requests[0].body;
    // The trailing dot must not be doubled.
    EXPECT_TRUE(body.find("www.example.com.<") != std::string::npos);
    EXPECT_TRUE(body.find("www.example.com..<") == std::string::npos);
}

TEST(Route53DriverTest, Update_MalformedSuccessXml_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "not valid xml at all");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

// ── validate (OPTIONAL yaddnsc_driver_validate entry) ────────────────────────
//
// Validation is a pure parse of driver_param against the driver's schema: a
// valid config passes; a missing required key and malformed JSON both map to
// YADDNSC_STATUS_INVALID_CONFIG. No HTTP exchange is queued or expected.

TEST(Route53DriverTest, Validate_ValidConfig_Succeeds) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, CONFIG);
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(Route53DriverTest, Validate_MissingRegion_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(
            fake, R"({"access_key_id":"AKID123","secret_access_key":"secret456","hosted_zone_id":"Z3M79L5CQABCDE","record_name":"www.example.com"})");
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(Route53DriverTest, Validate_MalformedJson_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({invalid)");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(fake.requests.empty());
}

TEST(Route53DriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr); // must be a no-op, must not crash
}
