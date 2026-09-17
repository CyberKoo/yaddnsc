//
// Unit tests for NamecheapDriver (driver/namecheap/)
//
// Verifies (through the v1 alpha ABI entries and FakeHostServices):
//   - descriptor returns expected metadata (name/version/author/capabilities).
//   - update builds the correct Namecheap DDNS API URL (GET, no body).
//   - update handles bare-domain (apex) records correctly.
//   - update rejects AAAA records with UNSUPPORTED_RECORD (no request sent).
//   - update with missing config fields returns INVALID_CONFIG.
//   - update succeeds for XML with ErrCount=0.
//   - update returns UPSTREAM_REJECTED for ErrCount>0 / malformed / empty XML.
// =============================================================================

#include <gtest/gtest.h>

#include "abi_test_harness.h"

// ── Shared fixtures ──────────────────────────────────────────────────────────

namespace {
    constexpr std::string_view CONFIG = R"({"password": "my-pass"})";

    /// Build a Namecheap API success XML response with the given IP.
    std::string make_success_xml(std::string_view ip) {
        return std::string{R"(<?xml version="1.0"?>
<interface-response>
  <Command>NAMEcheap.dynamicdns.update</Command>
  <ErrCount>0</ErrCount>
  <Done>true</Done>
  <IP>)"} + std::string{ip} + R"(</IP>
</interface-response>)";
    }

    /// Build a Namecheap API error XML response with the given error message.
    std::string make_error_xml(std::string_view err_msg) {
        return std::string{R"(<?xml version="1.0"?>
<interface-response>
  <Command>NAMEcheap.dynamicdns.update</Command>
  <ErrCount>1</ErrCount>
  <Done>true</Done>
  <errors>
    <error>)"} + std::string{err_msg} + R"(</error>
  </errors>
</interface-response>)";
    }
} // namespace

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(NamecheapDriverTest, Descriptor_ReturnsExpectedMetadata) {
    const yaddnsc_driver_descriptor *descriptor = nullptr;
    ASSERT_EQ(yaddnsc_driver_get_descriptor(&descriptor), YADDNSC_STATUS_OK);
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->magic, YADDNSC_DRIVER_MAGIC);
    EXPECT_EQ(descriptor->api_revision, YADDNSC_DRIVER_API_REVISION);
    EXPECT_EQ(std::string_view(descriptor->name.data, descriptor->name.size), "namecheap");
    EXPECT_EQ(std::string_view(descriptor->description.data, descriptor->description.size),
              "Updates DNS records via the Namecheap Dynamic DNS API");
    EXPECT_EQ(std::string_view(descriptor->author.data, descriptor->author.size), "Kotarou");
    EXPECT_EQ(std::string_view(descriptor->version.data, descriptor->version.size), "1.0.0");
    EXPECT_NE(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_A, 0u);
    EXPECT_EQ(descriptor->capabilities & YADDNSC_DRIVER_CAPABILITY_AAAA, 0u);
}

TEST(NamecheapDriverTest, Update_BasicARecord) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_xml("1.2.3.4"));

    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    ASSERT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    const auto &request = fake.requests[0];

    // Check URL
    EXPECT_EQ(request.url,
              "https://dynamicdns.park-your-domain.com/update?host=www&domain=example.com&password=my-pass&ip=1.2.3.4");

    // Check method — GET with no body
    EXPECT_EQ(request.method, YADDNSC_HTTP_GET);
    EXPECT_FALSE(request.body.has_value());
}

TEST(NamecheapDriverTest, Update_ApexDomain) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_xml("10.0.0.1"));

    const auto result = run_abi_update(fake, CONFIG, "10.0.0.1", "A", "example.com", "@", "example.com");
    ASSERT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;

    ASSERT_EQ(fake.requests.size(), 1u);
    EXPECT_TRUE(fake.requests[0].url.find("host=@") != std::string::npos)
        << "Apex domain should use @ as host, got URL: " << fake.requests[0].url;
}

TEST(NamecheapDriverTest, Update_AaaaRecord_ReturnsUnsupportedRecord) {
    FakeHostServices fake;
    const auto result = run_abi_update(fake, CONFIG, "::1", "AAAA", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UNSUPPORTED_RECORD);
    EXPECT_EQ(result.error_message,
              "Namecheap DDNS does not support AAAA (IPv6) records. Use an A record instead for domain "
              "'www.example.com'.");
    EXPECT_TRUE(fake.requests.empty());
}

TEST(NamecheapDriverTest, Update_MissingPassword_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result =
            run_abi_update(fake, R"({"not_password": "value"})", "1.2.3.4", "A", "example.com", "@", "example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(NamecheapDriverTest, Update_Success_ReturnsOk) {
    FakeHostServices fake;
    fake.queue_response(200, make_success_xml("1.2.3.4"));
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(NamecheapDriverTest, Update_Error_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, make_error_xml("Domain name not found"));
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(NamecheapDriverTest, Update_MultipleErrors_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, R"(<?xml version="1.0"?>
<interface-response>
  <ErrCount>2</ErrCount>
  <errors>
    <error>First error</error>
    <error>Second error</error>
  </errors>
</interface-response>)");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(NamecheapDriverTest, Update_MalformedXml_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "not xml");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(NamecheapDriverTest, Update_MissingErrCount_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, R"(<?xml version="1.0"?><interface-response><Done>true</Done></interface-response>)");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(NamecheapDriverTest, Update_EmptyBody_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(200, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(NamecheapDriverTest, Update_Non200_ReturnsUpstreamRejected) {
    FakeHostServices fake;
    fake.queue_response(500, "");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

TEST(NamecheapDriverTest, Update_ErrCountZero_WithoutIp_ReturnsOk) {
    // ErrCount=0 but no <IP> element — still a success (IP logging is best-effort).
    FakeHostServices fake;
    fake.queue_response(200, R"(<?xml version="1.0"?>
<interface-response>
  <ErrCount>0</ErrCount>
  <Done>true</Done>
</interface-response>)");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
}

TEST(NamecheapDriverTest, Update_Error_WithoutErrorMessages_ReturnsUpstreamRejected) {
    // ErrCount>0 but no <errors> children — falls back to the count-only log.
    FakeHostServices fake;
    fake.queue_response(200, R"(<?xml version="1.0"?>
<interface-response>
  <ErrCount>1</ErrCount>
  <Done>true</Done>
</interface-response>)");
    const auto result = run_abi_update(fake, CONFIG, "1.2.3.4", "A", "example.com", "www", "www.example.com");
    EXPECT_EQ(result.status, YADDNSC_STATUS_UPSTREAM_REJECTED);
}

// ── validate (OPTIONAL yaddnsc_driver_validate entry) ────────────────────────
//
// Validation is a pure parse of driver_param against the driver's schema: a
// valid config passes; a missing required key and malformed JSON both map to
// YADDNSC_STATUS_INVALID_CONFIG. No HTTP exchange is queued or expected.

TEST(NamecheapDriverTest, Validate_ValidConfig_Succeeds) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, CONFIG);
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(NamecheapDriverTest, Validate_MissingPassword_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({"not_password":"value"})");
    EXPECT_EQ(result.create_status, YADDNSC_STATUS_OK) << result.error_message;
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(result.error_message.starts_with("Driver configuration parse error:")) << result.error_message;
    EXPECT_TRUE(fake.requests.empty());
}

TEST(NamecheapDriverTest, Validate_MalformedJson_ReturnsInvalidConfig) {
    FakeHostServices fake;
    const auto result = run_abi_validate(fake, R"({invalid)");
    EXPECT_EQ(result.status, YADDNSC_STATUS_INVALID_CONFIG);
    EXPECT_TRUE(fake.requests.empty());
}

TEST(NamecheapDriverTest, Entries_NullArgumentsRejected) {
    EXPECT_EQ(yaddnsc_driver_get_descriptor(nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(yaddnsc_driver_update(nullptr, nullptr, nullptr), YADDNSC_STATUS_INVALID_ARGUMENT);
    yaddnsc_driver_destroy(nullptr); // must be a no-op, must not crash
}
