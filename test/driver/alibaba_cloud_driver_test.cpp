//
// Unit tests for AlibabaCloudDriver (driver/alibaba_cloud/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/version/author/capabilities).
//   - update builds the correct Alibaba Cloud API URL, method, content type.
//   - update body contains all required RPC parameters and the signature.
//   - update includes TTL parameter (default and custom).
//   - update URL-encodes special characters in parameters.
//   - update with missing config fields returns INVALID_CONFIG.
//   - update succeeds for HTTP 200 with RecordId (and even an empty object).
//   - update returns UPSTREAM_REJECTED for unparseable / non-200 responses.
// =============================================================================

#include <gtest/gtest.h>

#include "abi_test_harness.h"

// ── Shared fixtures ──────────────────────────────────────────────────────────

namespace {
    constexpr std::string_view CONFIG = R"({
        "access_key_id": "ak-id",
        "access_key_secret": "ak-secret",
        "record_id": "rec123"
    })";

    constexpr char SUCCESS_BODY[] = R"({"RequestId":"req123","RecordId":"rec456"})";
} // namespace

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(AlibabaCloudDriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor *descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "alibaba_cloud");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Updates DNS records via the Alibaba Cloud DNS API");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "1.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(AlibabaCloudDriverTest, Update_BasicARecord) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_BODY);

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto &request = fake.requests[0];

    // Check URL — Alibaba Cloud always uses the same API endpoint
    EXPECT_EQ(request.url, "https://alidns.aliyuncs.com/");

    // Check method and content type
    EXPECT_EQ(request.method, YADDNSC_HTTP_POST);
    EXPECT_EQ(request.content_type, "application/x-www-form-urlencoded");

    // Check body contains all required RPC parameters
    ASSERT_TRUE(request.body.has_value());
    const auto &body = *request.body;

    EXPECT_TRUE(body.find("Action=UpdateDomainRecord") != std::string::npos);
    EXPECT_TRUE(body.find("Format=JSON") != std::string::npos);
    EXPECT_TRUE(body.find("Version=2015-01-09") != std::string::npos);
    EXPECT_TRUE(body.find("AccessKeyId=ak-id") != std::string::npos);
    EXPECT_TRUE(body.find("SignatureMethod=HMAC-SHA1") != std::string::npos);
    EXPECT_TRUE(body.find("SignatureVersion=1.0") != std::string::npos);
    EXPECT_TRUE(body.find("RecordId=rec123") != std::string::npos);
    EXPECT_TRUE(body.find("RR=www") != std::string::npos);
    EXPECT_TRUE(body.find("Type=A") != std::string::npos);
    EXPECT_TRUE(body.find("Value=1.2.3.4") != std::string::npos);
    EXPECT_TRUE(body.find("TTL=600") != std::string::npos); // default TTL

    // Verify that signature-related parameters are present
    EXPECT_TRUE(body.find("SignatureNonce=") != std::string::npos);
    EXPECT_TRUE(body.find("Timestamp=") != std::string::npos);
    EXPECT_TRUE(body.find("Signature=") != std::string::npos);
}

TEST(AlibabaCloudDriverTest, Update_WithCustomTtl) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_BODY);

    const auto result = run_abi_update(fake,
                                       R"({"access_key_id":"ak-id","access_key_secret":"ak-secret","record_id":"rec123","ttl":120})",
                                       "10.0.0.1", "AAAA", "example.org", "@", "example.org");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    EXPECT_TRUE(fake.requests[0].body->find("TTL=120") != std::string::npos);
}

TEST(AlibabaCloudDriverTest, Update_ParametersAreUrlEncoded) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_BODY);

    const auto result = run_abi_update(fake,
                                       R"({"access_key_id":"ak/id+test","access_key_secret":"secret","record_id":"rec123"})",
                                       "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    // The access key ID contains special characters that should be URL-encoded
    const auto &body = *fake.requests[0].body;
    EXPECT_TRUE(body.find("ak%2Fid%2Btest") != std::string::npos)
        << "Special characters in AccessKeyId should be URL-encoded, body: " << body;
}

TEST(AlibabaCloudDriverTest, Update_MissingAccessKeyId_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"access_key_secret":"secret","record_id":"rec123"})", "1.2.3.4",
                                       "A", "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(AlibabaCloudDriverTest, Update_MissingRecordId_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"access_key_id":"ak-id","access_key_secret":"secret"})", "1.2.3.4",
                                       "A", "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(AlibabaCloudDriverTest, Update_200WithRecordId_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, SUCCESS_BODY);
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(AlibabaCloudDriverTest, Update_200UnparseableBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "not-json");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(AlibabaCloudDriverTest, Update_200UnexpectedShape_ReturnsOk) {
    // HTTP 200 with empty object — AlibabaUpdateResponse parses {} as
    // all-empty-string fields, which passes the parse check.
    // However, Alibaba Cloud always returns RequestId+RecordId on success,
    // so this is not a realistic response.
    FakeHostServices fake;
    fake.queue_response(200, R"({})");
    // {} parses to AlibabaUpdateResponse{request_id="", record_id=""},
    // which the driver considers successful (no error logged).
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(AlibabaCloudDriverTest, Update_Non200WithErrorBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, R"({"Code":"InvalidRecordId","Message":"The specified RecordId does not exist","RequestId":"req123"})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(AlibabaCloudDriverTest, Update_Non200UnparseableBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, "not-json");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(AlibabaCloudDriverTest, Update_Non200EmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(500, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(AlibabaCloudDriverTest, Update_TransportError_PropagatesStatus) {
    FakeHostServices fake;
    fake.queue_error(YADDNSC_STATUS_NETWORK_ERROR, "connection refused");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(result.error_message, "connection refused");
}

TEST(AlibabaCloudDriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr); // must be a no-op, must not crash
}
