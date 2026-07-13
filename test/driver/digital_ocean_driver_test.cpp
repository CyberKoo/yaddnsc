//
// Unit tests for DigitalOceanDriver (driver/digital_ocean/)
//
// Verifies:
//   - get_detail() returns expected metadata.
//   - generate_request() builds correct DigitalOcean API URL and headers.
//   - generate_request() produces valid JSON request body.
//   - generate_request() with missing config throws ParamParseException.
//   - check_response() returns true for valid domain_record response.
//   - check_response() returns false for error response.
//   - check_response() returns false for unparseable response.
// =============================================================================

#include <gtest/gtest.h>

#include "digital_ocean.h"
#include "config.hpp"
#include "response.hpp"
#include "factory_test_helpers.h"

TEST(DigitalOceanDriverTest, GetDetail_ReturnsExpectedMetadata) {
    DigitalOceanDriver driver;
    auto detail = driver.get_detail();
    EXPECT_EQ(detail.name, "digital_ocean");
    EXPECT_EQ(detail.description, "Updates DNS records via the DigitalOcean API");
    EXPECT_EQ(detail.author, "Kotarou");
    EXPECT_EQ(detail.version, "2.0.0");
}

TEST(DigitalOceanDriverTest, GenerateRequest_BasicARecord) {
    DigitalOceanDriver driver;
    DriverConfig config = R"({"record_id": "123456", "token": "my-token"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);

    // Check URL
    EXPECT_EQ(result.url,
              "https://api.digitalocean.com/v2/domains/example.com/records/123456");

    // Check method and content type
    EXPECT_EQ(result.request.method, DriverHttpMethod::PUT);
    EXPECT_EQ(result.request.content_type, "application/json");

    // Check auth header
    auto auth_it = result.request.headers.find("Authorization");
    ASSERT_NE(auth_it, result.request.headers.end());
    EXPECT_EQ(auth_it->second, "Bearer my-token");

    // Check body
    ASSERT_TRUE(result.request.body.has_value());
    auto &body = result.request.body.value();
    EXPECT_TRUE(body.find(R"("data":"1.2.3.4")") != std::string::npos);
}

TEST(DigitalOceanDriverTest, GenerateRequest_MissingRecordId_ThrowsParamParseException) {
    DigitalOceanDriver driver;
    DriverConfig config = R"({"token": "my-token"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(DigitalOceanDriverTest, GenerateRequest_MissingToken_ThrowsParamParseException) {
    DigitalOceanDriver driver;
    DriverConfig config = R"({"record_id": "123"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(DigitalOceanDriverTest, CheckResponse_Success_ReturnsTrue) {
    DigitalOceanDriver driver;
    HttpResponse resp{200, R"({
        "domain_record": {
            "id": 123456,
            "type": "A",
            "name": "www.example.com",
            "data": "1.2.3.4",
            "ttl": 300
        }
    })", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(DigitalOceanDriverTest, CheckResponse_SuccessWithAllFields_ReturnsTrue) {
    DigitalOceanDriver driver;
    HttpResponse resp{200, R"({
        "domain_record": {
            "id": 123456,
            "type": "AAAA",
            "name": "www.example.com",
            "data": "::1",
            "ttl": 120,
            "priority": 10,
            "port": 8080,
            "weight": 5,
            "flags": 0,
            "tag": null
        }
    })", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(DigitalOceanDriverTest, CheckResponse_Error_ReturnsFalse) {
    DigitalOceanDriver driver;
    HttpResponse resp{404, R"({
        "id": "not_found",
        "message": "The resource you were accessing could not be found."
    })", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(DigitalOceanDriverTest, CheckResponse_UnparseableBody_ReturnsFalse) {
    DigitalOceanDriver driver;
    HttpResponse resp{200, "not-json", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(DigitalOceanDriverTest, CheckResponse_EmptyBody_ReturnsFalse) {
    DigitalOceanDriver driver;
    HttpResponse resp{200, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(DigitalOceanDriverTest, CheckResponse_UnknownShape_ReturnsFalse) {
    // Body that is valid JSON but doesn't match any known shape.
    DigitalOceanDriver driver;
    HttpResponse resp{200, R"({"unknown_field": "value"})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(DigitalOceanDriverTest, FactoryCreateDestroy) { test_factory_create_destroy(); }
TEST(DigitalOceanDriverTest, FactoryMagic) { test_factory_magic(); }
TEST(DigitalOceanDriverTest, FactoryBuildId) { test_factory_build_id(); }
TEST(DigitalOceanDriverTest, FactoryCompilerIdHash) { test_factory_compiler_id_hash(); }
