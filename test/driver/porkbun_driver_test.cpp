//
// Unit tests for PorkbunDriver (driver/porkbun/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/version/author/capabilities).
//   - update builds the correct Porkbun API URL (editByNameType).
//   - update sets header auth keys (X-API-Key / X-Secret-API-Key).
//   - update produces JSON body with API keys and content.
//   - update handles empty @ subdomain (maps to "").
//   - update includes ttl when configured.
//   - update with missing config fields returns INVALID_CONFIG.
//   - update succeeds for status "SUCCESS" responses.
//   - update returns UPSTREAM_REJECTED for status "ERROR" / unparseable responses.
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
        "api_key": "pk1",
        "secret_api_key": "sk1"
    })";
}  // namespace

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(PorkbunDriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor* descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "porkbun");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Updates DNS records via the Porkbun API");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "1.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(PorkbunDriverTest, Update_BasicARecord) {
    FakeHostServices fake;
    fake.queue_response(200, R"({"status":"SUCCESS"})");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto& request = fake.requests[0];

    // Check URL
    EXPECT_EQ(request.url, "https://api.porkbun.com/api/json/v3/dns/editByNameType/example.com/A/www");

    // Check method and content type
    EXPECT_EQ(request.method, YADDNSC_HTTP_POST);
    EXPECT_EQ(request.content_type, "application/json");

    // Check header auth
    const auto key = request.header("X-API-Key");
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(*key, "pk1");
    const auto secret = request.header("X-Secret-API-Key");
    ASSERT_TRUE(secret.has_value());
    EXPECT_EQ(*secret, "sk1");

    // Check body contains API keys and content
    ASSERT_TRUE(request.body.has_value());
    const auto& body = *request.body;
    EXPECT_TRUE(body.find(R"("apikey":"pk1")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("secretapikey":"sk1")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("content":"1.2.3.4")") != std::string::npos);
}

TEST(PorkbunDriverTest, Update_SubdomainAt_BecomesEmpty) {
    FakeHostServices fake;
    fake.queue_response(200, R"({"status":"SUCCESS"})");

    const auto result = run_abi_update(fake, CONFIG, "10.0.0.1", "A", "example.com", "@", "example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    // When subdomain is "@" or empty, the URL path should be empty (root domain)
    ASSERT_EQ(fake.requests.size(), 1u);
    EXPECT_EQ(fake.requests[0].url, "https://api.porkbun.com/api/json/v3/dns/editByNameType/example.com/A/");
}

TEST(PorkbunDriverTest, Update_EmptySubdomain_BecomesEmpty) {
    FakeHostServices fake;
    fake.queue_response(200, R"({"status":"SUCCESS"})");

    const auto result = run_abi_update(fake, CONFIG, "10.0.0.1", "A", "example.com", "", "example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    EXPECT_EQ(fake.requests[0].url, "https://api.porkbun.com/api/json/v3/dns/editByNameType/example.com/A/");
}

TEST(PorkbunDriverTest, Update_WithTtl) {
    FakeHostServices fake;
    fake.queue_response(200, R"({"status":"SUCCESS"})");

    const auto result = run_abi_update(fake, R"({"api_key":"pk1","secret_api_key":"sk1","ttl":300})", "1.2.3.4", "A",
                                       "example.com", "www", "www.example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    ASSERT_TRUE(fake.requests[0].body.has_value());
    EXPECT_TRUE(fake.requests[0].body->find(R"("ttl":300)") != std::string::npos);
}

TEST(PorkbunDriverTest, Update_MissingApiKey_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result =
        run_abi_update(fake, R"({"secret_api_key":"sk1"})", "1.2.3.4", "A", "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(PorkbunDriverTest, Update_MissingSecretApiKey_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"api_key":"pk1"})", "1.2.3.4", "A", "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(PorkbunDriverTest, Update_Success_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, R"({"status":"SUCCESS"})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(PorkbunDriverTest, Update_SuccessWithMessage_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, R"({"status":"SUCCESS","message":"Record updated successfully"})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(PorkbunDriverTest, Update_ErrorWithoutCode_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, R"({"status":"ERROR","message":"Invalid API key"})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(PorkbunDriverTest, Update_ErrorWithCode_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(403, R"({"status":"ERROR","code":"ACCESS_DENIED","message":"Permission denied"})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(PorkbunDriverTest, Update_UnparseableBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "not-json");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(PorkbunDriverTest, Update_EmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(PorkbunDriverTest, Update_ErrorWithoutMessageOrCode_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(500, R"({"status":"ERROR"})");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

// ── validate (OPTIONAL yaddnsc_driver_validate entry) ────────────────────────
//
// Validation is a pure parse of driver_param against the driver's schema: a
// valid config passes; a missing required key and malformed JSON both map to
// YADDNSC_STATUS_INVALID_CONFIG. No HTTP exchange is queued or expected.

TEST(PorkbunDriverTest, Validate_ValidConfig_Succeeds) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, CONFIG);
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(PorkbunDriverTest, Validate_MissingSecretApiKey_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({"api_key":"pk1"})");
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(PorkbunDriverTest, Validate_MalformedJson_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({invalid)");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(fake.requests.empty());
}

TEST(PorkbunDriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr);  // must be a no-op, must not crash
}
