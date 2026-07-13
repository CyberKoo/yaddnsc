//
// Unit tests for VultrDriver (driver/vultr/)
//
// Verifies:
//   - get_detail() returns expected metadata.
//   - generate_request() builds correct Vultr API URL with domain/record_id.
//   - generate_request() sets Bearer authorization.
//   - generate_request() produces JSON body with name/data/ttl fields.
//   - generate_request() excludes ttl when not configured.
//   - generate_request() with missing config throws ParamParseException.
//   - check_response() returns true for HTTP 204.
//   - check_response() returns false for non-204 with error JSON.
//   - check_response() returns false for non-204 with empty body.
// =============================================================================

#include <gtest/gtest.h>

#include "vultr.h"
#include "config.hpp"
#include "response.hpp"
#include "factory_test_helpers.h"

TEST(VultrDriverTest, GetDetail_ReturnsExpectedMetadata) {
    VultrDriver driver;
    auto detail = driver.get_detail();
    EXPECT_EQ(detail.name, "vultr");
    EXPECT_EQ(detail.description, "Updates DNS records via the Vultr API");
    EXPECT_EQ(detail.author, "Kotarou");
    EXPECT_EQ(detail.version, "1.0.0");
}

TEST(VultrDriverTest, GenerateRequest_BasicARecord) {
    VultrDriver driver;
    DriverConfig config = R"({
        "api_key": "my-key",
        "record_id": "rec123"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);

    // Check URL
    EXPECT_EQ(result.url,
              "https://api.vultr.com/v2/domains/example.com/records/rec123");

    // Check method and content type
    EXPECT_EQ(result.request.method, DriverHttpMethod::PATCH);
    EXPECT_EQ(result.request.content_type, "application/json");

    // Check auth header
    auto auth_it = result.request.headers.find("Authorization");
    ASSERT_NE(auth_it, result.request.headers.end());
    EXPECT_EQ(auth_it->second, "Bearer my-key");

    // Check body
    ASSERT_TRUE(result.request.body.has_value());
    auto &body = result.request.body.value();
    EXPECT_TRUE(body.find(R"("name":"www")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("data":"1.2.3.4")") != std::string::npos);
    // ttl should be omitted when not configured
    EXPECT_TRUE(body.find("ttl") == std::string::npos);
}

TEST(VultrDriverTest, GenerateRequest_WithTtl) {
    VultrDriver driver;
    DriverConfig config = R"({
        "api_key": "my-key",
        "record_id": "rec123",
        "ttl": 600
    })";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "AAAA",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };

    auto result = driver.generate_request(config, ctx);
    ASSERT_TRUE(result.request.body.has_value());
    EXPECT_TRUE(result.request.body.value().find(R"("ttl":600)") != std::string::npos);
}

TEST(VultrDriverTest, GenerateRequest_MissingApiKey_ThrowsParamParseException) {
    VultrDriver driver;
    DriverConfig config = R"({"record_id": "rec123"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(VultrDriverTest, GenerateRequest_MissingRecordId_ThrowsParamParseException) {
    VultrDriver driver;
    DriverConfig config = R"({"api_key": "my-key"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(VultrDriverTest, CheckResponse_204_ReturnsTrue) {
    VultrDriver driver;
    HttpResponse resp{204, "", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(VultrDriverTest, CheckResponse_Non204_WithErrorBody_ReturnsFalse) {
    VultrDriver driver;
    HttpResponse resp{400, R"({"errors":[{"detail":"Invalid record ID"}]})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(VultrDriverTest, CheckResponse_Non204_WithMultipleErrors_ReturnsFalse) {
    VultrDriver driver;
    HttpResponse resp{400, R"({
        "errors": [
            {"detail": "Invalid API key"},
            {"detail": "Rate limit exceeded"}
        ]
    })", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(VultrDriverTest, CheckResponse_Non204_UnparseableBody_ReturnsFalse) {
    VultrDriver driver;
    HttpResponse resp{400, "not-json", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(VultrDriverTest, CheckResponse_Non204_EmptyBody_ReturnsFalse) {
    VultrDriver driver;
    HttpResponse resp{500, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(VultrDriverTest, CheckResponse_Non204_WithError_SuccessStatusFalse) {
    // Even with 200 status, Vultr returns 204 on success.
    // But 200 with empty body is not expected — treat as failure.
    VultrDriver driver;
    HttpResponse resp{200, "something", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(VultrDriverTest, FactoryCreateDestroy) { test_factory_create_destroy(); }
TEST(VultrDriverTest, FactoryMagic) { test_factory_magic(); }
TEST(VultrDriverTest, FactoryBuildId) { test_factory_build_id(); }
TEST(VultrDriverTest, FactoryCompilerIdHash) { test_factory_compiler_id_hash(); }

TEST(VultrDriverTest, CheckResponse_Non204_NoRelevantErrorKey_ReturnsFalse) {
    VultrDriver driver;
    HttpResponse resp{400, R"({"some_other_key": "value"})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}
