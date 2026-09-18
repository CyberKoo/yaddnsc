//
// Unit tests for SimpleDriver (driver/simple/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/version/author/capabilities).
//   - update substitutes URL template variables (context + config params) correctly.
//   - update with missing/non-object/non-string url config returns INVALID_CONFIG.
//   - update succeeds for 2xx responses with a non-empty body.
//   - update returns UPSTREAM_REJECTED for 3xx/4xx/5xx status codes.
//   - update returns UPSTREAM_REJECTED for 2xx responses with an empty body.
// =============================================================================

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/driver_abi.h>

#include "abi_test_harness.h"

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(SimpleDriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor* descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "simple");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Generic HTTP driver with URL template substitution");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "2.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(SimpleDriverTest, Update_BasicUrlTemplate) {
    FakeHostServices fake;
    fake.queue_response(200, "update successful");

    const auto result = run_abi_update(fake,
                                       R"({
        "url": "https://dns.example.com/update?ip={ip_addr}&domain={fqdn}",
        "custom_param": "hello"
    })",
                                       "192.168.1.1", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto& request = fake.requests[0];
    EXPECT_EQ(request.url, "https://dns.example.com/update?ip=192.168.1.1&domain=www.example.com");
    EXPECT_EQ(request.method, YADDNSC_HTTP_GET);
    EXPECT_FALSE(request.body.has_value());
    EXPECT_TRUE(request.content_type.empty());
}

TEST(SimpleDriverTest, Update_TemplateWithAllContextKeys) {
    FakeHostServices fake;
    fake.queue_response(200, "update successful");

    const auto result = run_abi_update(fake,
                                       R"({
        "url": "https://dns.example.com/{ip_addr}/{rd_type}/{domain}/{subdomain}/{fqdn}"
    })",
                                       "10.0.0.1", "AAAA", "test.org", "@", "test.org");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    EXPECT_EQ(fake.requests[0].url, "https://dns.example.com/10.0.0.1/AAAA/test.org/@/test.org");
}

TEST(SimpleDriverTest, Update_CustomParamsAreSubstituted) {
    FakeHostServices fake;
    fake.queue_response(200, "update successful");

    const auto result = run_abi_update(fake,
                                       R"({
        "url": "https://api.example.com/{token}?host={host}",
        "token": "my-secret-token",
        "host": "my-host"
    })",
                                       "1.2.3.4", "A", "example.com", "@", "example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    EXPECT_EQ(fake.requests[0].url, "https://api.example.com/my-secret-token?host=my-host");
}

TEST(SimpleDriverTest, Update_ConfigParamOverridesUrlToken) {
    // Custom config params are substituted before context params,
    // so config values can fill template slots.
    FakeHostServices fake;
    fake.queue_response(200, "update successful");

    const auto result = run_abi_update(fake,
                                       R"({
        "url": "https://{domain}/update?ip={ip_addr}",
        "domain": "override.example.com"
    })",
                                       "10.0.0.1", "A", "original.com", "@", "original.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    // "domain" appears in config, so it's substituted BEFORE context substitution.
    // The config param "domain" is substituted first, then the context "domain".
    // But since "domain" was already in config, and {domain} was replaced with
    // "override.example.com", the context substitution for {domain} won't find it.
    EXPECT_EQ(fake.requests[0].url, "https://override.example.com/update?ip=10.0.0.1");
}

TEST(SimpleDriverTest, Update_MissingUrl_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result =
        run_abi_update(fake, R"({"not_url": "value"})", "1.2.3.4", "A", "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(SimpleDriverTest, Update_EmptyConfig_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, "{}", "1.2.3.4", "A", "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(SimpleDriverTest, Update_NonObjectConfig_ReturnsInvalidConfig) {
    FakeHostServices fake;
    // A JSON value that is not an object → rejected by generate_request.
    const auto result = run_abi_update(fake, R"("just a string")", "1.2.3.4", "A", "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(SimpleDriverTest, Update_NonStringUrl_ReturnsInvalidConfig) {
    FakeHostServices fake;
    // "url" present but not a string → rejected.
    const auto result = run_abi_update(fake, R"({"url": 12345})", "1.2.3.4", "A", "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(SimpleDriverTest, Update_NonStringConfigValue_IsSkipped) {
    FakeHostServices fake;
    fake.queue_response(200, "update successful");

    // Non-string config values should be skipped during substitution.
    const auto result = run_abi_update(fake, R"({"url": "https://example.com/{ip_addr}", "ttl": 300})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    EXPECT_EQ(fake.requests[0].url, "https://example.com/1.2.3.4");
}

TEST(SimpleDriverTest, Update_2xxWithBody_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, "update successful");
    const auto result = run_abi_update(fake, R"({"url": "https://example.com/update?ip={ip_addr}"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(SimpleDriverTest, Update_2xxWithEmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "");
    const auto result = run_abi_update(fake, R"({"url": "https://example.com/update?ip={ip_addr}"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(SimpleDriverTest, Update_204WithEmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(204, "");
    const auto result = run_abi_update(fake, R"({"url": "https://example.com/update?ip={ip_addr}"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(SimpleDriverTest, Update_3xx_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(301, "redirect");
    const auto result = run_abi_update(fake, R"({"url": "https://example.com/update?ip={ip_addr}"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(SimpleDriverTest, Update_4xx_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(404, "not found");
    const auto result = run_abi_update(fake, R"({"url": "https://example.com/update?ip={ip_addr}"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(SimpleDriverTest, Update_5xx_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(500, "server error");
    const auto result = run_abi_update(fake, R"({"url": "https://example.com/update?ip={ip_addr}"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(SimpleDriverTest, Update_Non2xxWithEmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(400, "");
    const auto result = run_abi_update(fake, R"({"url": "https://example.com/update?ip={ip_addr}"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(SimpleDriverTest, Update_TransportError_PropagatesStatus) {
    FakeHostServices fake;
    fake.queue_error(YADDNSC_STATUS_NETWORK_ERROR, "connection refused");
    const auto result = run_abi_update(fake, R"({"url": "https://example.com/update?ip={ip_addr}"})", "1.2.3.4", "A",
                                       "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_NETWORK_ERROR);
    EXPECT_EQ(result.error_message, "connection refused");
}

// ── validate (OPTIONAL yaddnsc_driver_validate entry) ────────────────────────
//
// Validation is a pure parse of driver_param against the driver's schema: a
// valid config passes; a missing required key and malformed JSON both map to
// YADDNSC_STATUS_INVALID_CONFIG. No HTTP exchange is queued or expected.

TEST(SimpleDriverTest, Validate_ValidConfig_Succeeds) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({"url":"https://dns.example.com/update?ip={ip_addr}"})");
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(SimpleDriverTest, Validate_MissingUrl_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({"custom_param":"hello"})");
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(SimpleDriverTest, Validate_MalformedJson_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({invalid)");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(fake.requests.empty());
}

TEST(SimpleDriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr);  // must be a no-op, must not crash
}
