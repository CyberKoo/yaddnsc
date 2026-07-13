//
// Unit tests for DuckDnsDriver (driver/duckdns/)
//
// Verifies:
//   - get_detail() returns expected metadata.
//   - generate_request() builds correct DuckDNS API URL.
//   - generate_request() uses ipv6 param for AAAA records.
//   - generate_request() appends verbose flag when configured.
//   - generate_request() with missing token throws ParamParseException.
//   - check_response() returns true for "OK" body.
//   - check_response() returns true for verbose "OK\n..." body.
//   - check_response() returns false for "KO".
//   - check_response() returns false for empty body.
// =============================================================================

#include <gtest/gtest.h>

#include "duckdns.h"
#include "config.hpp"
#include "factory_test_helpers.h"

TEST(DuckDnsDriverTest, GetDetail_ReturnsExpectedMetadata) {
    DuckDnsDriver driver;
    auto detail = driver.get_detail();
    EXPECT_EQ(detail.name, "duckdns");
    EXPECT_EQ(detail.description, "Updates DNS records via the DuckDNS API");
    EXPECT_EQ(detail.author, "Kotarou");
    EXPECT_EQ(detail.version, "1.0.0");
}

TEST(DuckDnsDriverTest, GenerateRequest_BasicARecord) {
    DuckDnsDriver driver;
    DriverConfig config = R"({"token": "my-token"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "duckdns.org", .subdomain = "mydomain", .fqdn = "mydomain.duckdns.org"
    };

    auto result = driver.generate_request(config, ctx);
    EXPECT_EQ(result.url,
              "https://www.duckdns.org/update?domains=mydomain&token=my-token&ip=1.2.3.4");
    EXPECT_EQ(result.request.method, DriverHttpMethod::GET);
    EXPECT_FALSE(result.request.body.has_value());
}

TEST(DuckDnsDriverTest, GenerateRequest_AAAARecord_UsesIpv6Param) {
    DuckDnsDriver driver;
    DriverConfig config = R"({"token": "my-token"})";
    DriverUpdateParams ctx{
        .ip_addr = "::1", .rd_type = "AAAA",
        .domain = "duckdns.org", .subdomain = "mydomain", .fqdn = "mydomain.duckdns.org"
    };

    auto result = driver.generate_request(config, ctx);
    EXPECT_TRUE(result.url.find("ipv6=%3A%3A1") != std::string_view::npos ||
                result.url.find("ipv6=::1") != std::string_view::npos)
        << "AAAA record should use ipv6 parameter, got URL: " << result.url;
}

TEST(DuckDnsDriverTest, GenerateRequest_VerboseMode_AppendsVerboseFlag) {
    DuckDnsDriver driver;
    DriverConfig config = R"({"token": "my-token", "verbose": true})";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "A",
        .domain = "duckdns.org", .subdomain = "test", .fqdn = "test.duckdns.org"
    };

    auto result = driver.generate_request(config, ctx);
    EXPECT_TRUE(result.url.find("&verbose=true") != std::string_view::npos)
        << "Verbose mode should append &verbose=true, got URL: " << result.url;
}

TEST(DuckDnsDriverTest, GenerateRequest_VerboseFalse_DoesNotAppendVerboseFlag) {
    DuckDnsDriver driver;
    DriverConfig config = R"({"token": "my-token", "verbose": false})";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "A",
        .domain = "duckdns.org", .subdomain = "test", .fqdn = "test.duckdns.org"
    };

    auto result = driver.generate_request(config, ctx);
    EXPECT_TRUE(result.url.find("&verbose=true") == std::string_view::npos)
        << "Verbose false should NOT append &verbose=true, got URL: " << result.url;
}

TEST(DuckDnsDriverTest, GenerateRequest_MissingToken_ThrowsParamParseException) {
    DuckDnsDriver driver;
    DriverConfig config = R"({"not_token": "value"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "duckdns.org", .subdomain = "x", .fqdn = "x.duckdns.org"
    };

    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(DuckDnsDriverTest, CheckResponse_OkBody_ReturnsTrue) {
    DuckDnsDriver driver;
    HttpResponse resp{200, "OK", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(DuckDnsDriverTest, CheckResponse_VerboseOkBody_ReturnsTrue) {
    DuckDnsDriver driver;
    HttpResponse resp{200, "OK\n127.0.0.1\nupdated successfully", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(DuckDnsDriverTest, CheckResponse_KoBody_ReturnsFalse) {
    DuckDnsDriver driver;
    HttpResponse resp{200, "KO", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(DuckDnsDriverTest, CheckResponse_EmptyBody_ReturnsFalse) {
    DuckDnsDriver driver;
    HttpResponse resp{200, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(DuckDnsDriverTest, CheckResponse_ErrorStatusWithOkBody_ReturnsTrue) {
    // DuckDNS check_response reads the body first, not the status code.
    // Even with a 500 status, a body starting with "OK" is treated as success.
    DuckDnsDriver driver;
    HttpResponse resp{500, "OK", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(DuckDnsDriverTest, CheckResponse_ErrorStatusWithNonOkBody_ReturnsFalse) {
    DuckDnsDriver driver;
    HttpResponse resp{500, "Internal Server Error", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(DuckDnsDriverTest, FactoryCreateDestroy) { test_factory_create_destroy(); }
TEST(DuckDnsDriverTest, FactoryMagic) { test_factory_magic(); }
TEST(DuckDnsDriverTest, FactoryBuildId) { test_factory_build_id(); }
TEST(DuckDnsDriverTest, FactoryCompilerIdHash) { test_factory_compiler_id_hash(); }
