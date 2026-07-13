//
// Unit tests for PorkbunDriver (driver/porkbun/)
//
// Verifies:
//   - get_detail() returns expected metadata.
//   - generate_request() builds correct Porkbun API URL.
//   - generate_request() sets header auth keys.
//   - generate_request() produces JSON body with API keys and content.
//   - generate_request() handles empty @ subdomain (maps to "").
//   - generate_request() includes ttl when configured.
//   - generate_request() with missing config throws ParamParseException.
//   - check_response() returns true for status "SUCCESS".
//   - check_response() returns false for status "ERROR" with message.
//   - check_response() returns false for unparseable response.
// =============================================================================

#include <gtest/gtest.h>

#include "porkbun.h"
#include "config.hpp"
#include "response.hpp"
#include "factory_test_helpers.h"

TEST(PorkbunDriverTest, GetDetail_ReturnsExpectedMetadata) {
    PorkbunDriver driver;
    auto detail = driver.get_detail();
    EXPECT_EQ(detail.name, "porkbun");
    EXPECT_EQ(detail.description, "Updates DNS records via the Porkbun API");
    EXPECT_EQ(detail.author, "Kotarou");
    EXPECT_EQ(detail.version, "1.0.0");
}

TEST(PorkbunDriverTest, GenerateRequest_BasicARecord) {
    PorkbunDriver driver;
    DriverConfig config = R"({
        "api_key": "pk1",
        "secret_api_key": "sk1"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);

    // Check URL
    EXPECT_EQ(result.url,
              "https://api.porkbun.com/api/json/v3/dns/editByNameType/example.com/A/www");

    // Check method and content type
    EXPECT_EQ(result.request.method, DriverHttpMethod::POST);
    EXPECT_EQ(result.request.content_type, "application/json");

    // Check header auth
    auto key_it = result.request.headers.find("X-API-Key");
    ASSERT_NE(key_it, result.request.headers.end());
    EXPECT_EQ(key_it->second, "pk1");
    auto secret_it = result.request.headers.find("X-Secret-API-Key");
    ASSERT_NE(secret_it, result.request.headers.end());
    EXPECT_EQ(secret_it->second, "sk1");

    // Check body contains API keys and content
    ASSERT_TRUE(result.request.body.has_value());
    auto &body = result.request.body.value();
    EXPECT_TRUE(body.find(R"("apikey":"pk1")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("secretapikey":"sk1")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("content":"1.2.3.4")") != std::string::npos);
}

TEST(PorkbunDriverTest, GenerateRequest_SubdomainAt_BecomesEmpty) {
    PorkbunDriver driver;
    DriverConfig config = R"({"api_key": "pk1", "secret_api_key": "sk1"})";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };

    auto result = driver.generate_request(config, ctx);
    // When subdomain is "@" or empty, the URL path should be empty (root domain)
    EXPECT_EQ(result.url,
              "https://api.porkbun.com/api/json/v3/dns/editByNameType/example.com/A/");
}

TEST(PorkbunDriverTest, GenerateRequest_EmptySubdomain_BecomesEmpty) {
    PorkbunDriver driver;
    DriverConfig config = R"({"api_key": "pk1", "secret_api_key": "sk1"})";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "A",
        .domain = "example.com", .subdomain = "", .fqdn = "example.com"
    };

    auto result = driver.generate_request(config, ctx);
    EXPECT_EQ(result.url,
              "https://api.porkbun.com/api/json/v3/dns/editByNameType/example.com/A/");
}

TEST(PorkbunDriverTest, GenerateRequest_WithTtl) {
    PorkbunDriver driver;
    DriverConfig config = R"({
        "api_key": "pk1",
        "secret_api_key": "sk1",
        "ttl": 300
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);
    ASSERT_TRUE(result.request.body.has_value());
    EXPECT_TRUE(result.request.body.value().find(R"("ttl":300)") != std::string::npos);
}

TEST(PorkbunDriverTest, GenerateRequest_MissingApiKey_ThrowsParamParseException) {
    PorkbunDriver driver;
    DriverConfig config = R"({"secret_api_key": "sk1"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(PorkbunDriverTest, GenerateRequest_MissingSecretApiKey_ThrowsParamParseException) {
    PorkbunDriver driver;
    DriverConfig config = R"({"api_key": "pk1"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(PorkbunDriverTest, CheckResponse_Success_ReturnsTrue) {
    PorkbunDriver driver;
    HttpResponse resp{200, R"({"status":"SUCCESS"})", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(PorkbunDriverTest, CheckResponse_SuccessWithMessage_ReturnsTrue) {
    PorkbunDriver driver;
    HttpResponse resp{200, R"({"status":"SUCCESS","message":"Record updated successfully"})", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(PorkbunDriverTest, CheckResponse_ErrorWithoutCode_ReturnsFalse) {
    PorkbunDriver driver;
    HttpResponse resp{200, R"({"status":"ERROR","message":"Invalid API key"})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(PorkbunDriverTest, CheckResponse_ErrorWithCode_ReturnsFalse) {
    PorkbunDriver driver;
    HttpResponse resp{403, R"({"status":"ERROR","code":"ACCESS_DENIED","message":"Permission denied"})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(PorkbunDriverTest, CheckResponse_UnparseableBody_ReturnsFalse) {
    PorkbunDriver driver;
    HttpResponse resp{200, "not-json", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(PorkbunDriverTest, CheckResponse_EmptyBody_ReturnsFalse) {
    PorkbunDriver driver;
    HttpResponse resp{200, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(PorkbunDriverTest, FactoryCreateDestroy) { test_factory_create_destroy(); }
TEST(PorkbunDriverTest, FactoryMagic) { test_factory_magic(); }
TEST(PorkbunDriverTest, FactoryBuildId) { test_factory_build_id(); }
TEST(PorkbunDriverTest, FactoryCompilerIdHash) { test_factory_compiler_id_hash(); }

TEST(PorkbunDriverTest, CheckResponse_ErrorWithoutMessageOrCode_ReturnsFalse) {
    PorkbunDriver driver;
    HttpResponse resp{500, R"({"status":"ERROR"})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}
