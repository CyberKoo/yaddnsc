//
// Unit tests for DuckDnsDriver (driver/duckdns/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/description/author/capabilities).
//   - update builds the correct DuckDNS API URL (ip param for A records).
//   - update uses the ipv6 param for AAAA records.
//   - update appends &verbose=true only when verbose is enabled.
//   - update with a missing token returns INVALID_CONFIG.
//   - update succeeds for "OK" and verbose "OK\n..." bodies (even on HTTP 500).
//   - update returns UPSTREAM_REJECTED for "KO", empty, or non-OK bodies.
// =============================================================================

#include <gtest/gtest.h>

#include "abi_test_harness.h"

namespace {
    constexpr std::string_view CONFIG = R"({"token": "my-token"})";
}

TEST(DuckDnsDriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor *descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "duckdns");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Updates DNS records via the DuckDNS API");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "1.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(DuckDnsDriverTest, Update_BasicARecord) {
    FakeHostServices fake;
    fake.queue_response(200, "OK");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "duckdns.org", "mydomain",
                                       "mydomain.duckdns.org");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto &request = fake.requests[0];

    EXPECT_EQ(request.url, "https://www.duckdns.org/update?domains=mydomain&token=my-token&ip=1.2.3.4");
    EXPECT_EQ(request.method, YADDNSC_HTTP_GET);
    EXPECT_FALSE(request.body.has_value());
}

TEST(DuckDnsDriverTest, Update_AAAARecord_UsesIpv6Param) {
    FakeHostServices fake;
    fake.queue_response(200, "OK");

    const auto result = run_abi_update(fake, CONFIG, "::1", "AAAA", "duckdns.org", "mydomain",
                                       "mydomain.duckdns.org");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    EXPECT_TRUE(fake.requests[0].url.find("ipv6=%3A%3A1") != std::string::npos ||
                fake.requests[0].url.find("ipv6=::1") != std::string::npos)
        << "AAAA record should use ipv6 parameter, got URL: " << fake.requests[0].url;
}

TEST(DuckDnsDriverTest, Update_VerboseMode_AppendsVerboseFlag) {
    FakeHostServices fake;
    fake.queue_response(200, "OK");

    const auto result = run_abi_update(fake, R"({"token": "my-token", "verbose": true})", "10.0.0.1", "A",
                                       "duckdns.org", "test", "test.duckdns.org");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    EXPECT_TRUE(fake.requests[0].url.find("&verbose=true") != std::string::npos)
        << "Verbose mode should append &verbose=true, got URL: " << fake.requests[0].url;
}

TEST(DuckDnsDriverTest, Update_VerboseFalse_DoesNotAppendVerboseFlag) {
    FakeHostServices fake;
    fake.queue_response(200, "OK");

    const auto result = run_abi_update(fake, R"({"token": "my-token", "verbose": false})", "10.0.0.1", "A",
                                       "duckdns.org", "test", "test.duckdns.org");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    EXPECT_TRUE(fake.requests[0].url.find("&verbose=true") == std::string::npos)
        << "Verbose false should NOT append &verbose=true, got URL: " << fake.requests[0].url;
}

TEST(DuckDnsDriverTest, Update_MissingToken_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, R"({"not_token": "value"})", "1.2.3.4", "A", "duckdns.org", "x",
                                       "x.duckdns.org");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(DuckDnsDriverTest, Update_OkBody_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, "OK");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "duckdns.org", "mydomain",
                                       "mydomain.duckdns.org");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(DuckDnsDriverTest, Update_VerboseOkBody_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, "OK\n127.0.0.1\nupdated successfully");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "duckdns.org", "mydomain",
                                       "mydomain.duckdns.org");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(DuckDnsDriverTest, Update_KoBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "KO");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "duckdns.org", "mydomain",
                                       "mydomain.duckdns.org");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(DuckDnsDriverTest, Update_EmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "duckdns.org", "mydomain",
                                       "mydomain.duckdns.org");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(DuckDnsDriverTest, Update_ErrorStatusWithOkBody_ReturnsOk) {
    // DuckDNS check_response reads the body first, not the status code.
    // Even with a 500 status, a body starting with "OK" is treated as success.
    FakeHostServices fake;
    fake.queue_response(500, "OK");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "duckdns.org", "mydomain",
                                       "mydomain.duckdns.org");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(DuckDnsDriverTest, Update_ErrorStatusWithNonOkBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(500, "Internal Server Error");

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "duckdns.org", "mydomain",
                                       "mydomain.duckdns.org");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(DuckDnsDriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr); // must be a no-op, must not crash
}
