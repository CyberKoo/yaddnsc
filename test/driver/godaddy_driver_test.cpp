//
// Unit tests for GoDaddyDriver (driver/godaddy/)
//
// Verifies:
//   - get_detail() returns expected metadata.
//   - generate_request() builds correct GoDaddy API URL with domain/type/name.
//   - generate_request() sets sso-key Authorization header.
//   - generate_request() produces JSON array body with single record.
//   - generate_request() uses configurable TTL.
//   - generate_request() with missing config throws ParamParseException.
//   - check_response() returns true for HTTP 200.
//   - check_response() returns false for non-200 status codes.
// =============================================================================

#include <gtest/gtest.h>

#include "godaddy.h"
#include "config.hpp"
#include "factory_test_helpers.h"

TEST(GoDaddyDriverTest, GetDetail_ReturnsExpectedMetadata) {
    GoDaddyDriver driver;
    auto detail = driver.get_detail();
    EXPECT_EQ(detail.name, "godaddy");
    EXPECT_EQ(detail.description, "Updates DNS records via the GoDaddy API");
    EXPECT_EQ(detail.author, "Kotarou");
    EXPECT_EQ(detail.version, "1.0.0");
}

TEST(GoDaddyDriverTest, GenerateRequest_BasicARecord) {
    GoDaddyDriver driver;
    DriverConfig config = R"({"key": "mykey", "secret": "mysecret"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);

    // Check URL
    EXPECT_EQ(result.url,
              "https://api.godaddy.com/v1/domains/example.com/records/A/www");

    // Check method and content type
    EXPECT_EQ(result.request.method, DriverHttpMethod::PUT);
    EXPECT_EQ(result.request.content_type, "application/json");

    // Check auth header (sso-key)
    auto auth_it = result.request.headers.find("Authorization");
    ASSERT_NE(auth_it, result.request.headers.end());
    EXPECT_EQ(auth_it->second, "sso-key mykey:mysecret");

    // Check body: GoDaddy expects an array with a single record
    ASSERT_TRUE(result.request.body.has_value());
    auto &body = result.request.body.value();
    EXPECT_TRUE(body.starts_with("["));
    EXPECT_TRUE(body.ends_with("]"));
    EXPECT_TRUE(body.find(R"("data":"1.2.3.4")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("type":"A")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("ttl":600)") != std::string::npos);
}

TEST(GoDaddyDriverTest, GenerateRequest_WithCustomTtl) {
    GoDaddyDriver driver;
    DriverConfig config = R"({"key": "mykey", "secret": "mysecret", "ttl": 1200})";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "AAAA",
        .domain = "example.org", .subdomain = "@", .fqdn = "example.org"
    };

    auto result = driver.generate_request(config, ctx);
    ASSERT_TRUE(result.request.body.has_value());
    EXPECT_TRUE(result.request.body.value().find(R"("ttl":1200)") != std::string::npos);
}

TEST(GoDaddyDriverTest, GenerateRequest_MissingKey_ThrowsParamParseException) {
    GoDaddyDriver driver;
    DriverConfig config = R"({"secret": "mysecret"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(GoDaddyDriverTest, GenerateRequest_MissingSecret_ThrowsParamParseException) {
    GoDaddyDriver driver;
    DriverConfig config = R"({"key": "mykey"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(GoDaddyDriverTest, CheckResponse_200_ReturnsTrue) {
    GoDaddyDriver driver;
    HttpResponse resp{200, "", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(GoDaddyDriverTest, CheckResponse_200_WithBody_ReturnsTrue) {
    GoDaddyDriver driver;
    HttpResponse resp{200, "some body", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(GoDaddyDriverTest, CheckResponse_Non200_ReturnsFalse) {
    GoDaddyDriver driver;
    HttpResponse resp{400, R"({"message":"Bad Request"})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(GoDaddyDriverTest, CheckResponse_Non200_EmptyBody_ReturnsFalse) {
    GoDaddyDriver driver;
    HttpResponse resp{500, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(GoDaddyDriverTest, CheckResponse_Non200_WithBody_ReturnsFalse) {
    GoDaddyDriver driver;
    HttpResponse resp{403, R"({"message":"Forbidden"})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(GoDaddyDriverTest, FactoryCreateDestroy) { test_factory_create_destroy(); }
TEST(GoDaddyDriverTest, FactoryMagic) { test_factory_magic(); }
TEST(GoDaddyDriverTest, FactoryBuildId) { test_factory_build_id(); }
TEST(GoDaddyDriverTest, FactoryCompilerIdHash) { test_factory_compiler_id_hash(); }
