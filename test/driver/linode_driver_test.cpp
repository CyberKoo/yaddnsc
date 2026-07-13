//
// Unit tests for LinodeDriver (driver/linode/)
//
// Verifies:
//   - get_detail() returns expected metadata.
//   - generate_request() builds correct Linode API URL.
//   - generate_request() sets Bearer authorization.
//   - generate_request() produces JSON request body with name/target.
//   - generate_request() includes ttl_sec when configured.
//   - generate_request() with missing config throws ParamParseException.
//   - check_response() returns true for HTTP 200.
//   - check_response() returns false for non-200 with error JSON.
//   - check_response() returns false for non-200 with empty body.
// =============================================================================

#include <gtest/gtest.h>

#include "linode.h"
#include "config.hpp"
#include "response.hpp"
#include "factory_test_helpers.h"

TEST(LinodeDriverTest, GetDetail_ReturnsExpectedMetadata) {
    LinodeDriver driver;
    auto detail = driver.get_detail();
    EXPECT_EQ(detail.name, "linode");
    EXPECT_EQ(detail.description, "Updates DNS records via the Linode API");
    EXPECT_EQ(detail.author, "Kotarou");
    EXPECT_EQ(detail.version, "1.0.0");
}

TEST(LinodeDriverTest, GenerateRequest_BasicARecord) {
    LinodeDriver driver;
    DriverConfig config = R"({
        "token": "my-token",
        "domain_id": "dom123",
        "record_id": "rec456"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);

    // Check URL
    EXPECT_EQ(result.url,
              "https://api.linode.com/v4/domains/dom123/records/rec456");

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
    EXPECT_TRUE(body.find(R"("name":"www")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("target":"1.2.3.4")") != std::string::npos);
    // ttl_sec is optional and should be omitted when not set
    EXPECT_TRUE(body.find("ttl_sec") == std::string::npos);
}

TEST(LinodeDriverTest, GenerateRequest_WithTtlSec) {
    LinodeDriver driver;
    DriverConfig config = R"({
        "token": "t1",
        "domain_id": "d1",
        "record_id": "r1",
        "ttl_sec": 3600
    })";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "AAAA",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };

    auto result = driver.generate_request(config, ctx);
    ASSERT_TRUE(result.request.body.has_value());
    EXPECT_TRUE(result.request.body.value().find(R"("ttl_sec":3600)") != std::string::npos);
}

TEST(LinodeDriverTest, GenerateRequest_MissingToken_ThrowsParamParseException) {
    LinodeDriver driver;
    DriverConfig config = R"({"domain_id": "d1", "record_id": "r1"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(LinodeDriverTest, GenerateRequest_MissingDomainId_ThrowsParamParseException) {
    LinodeDriver driver;
    DriverConfig config = R"({"token": "t1", "record_id": "r1"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(LinodeDriverTest, CheckResponse_200_ReturnsTrue) {
    LinodeDriver driver;
    HttpResponse resp{200, R"({"id": 123, "type": "A", "name": "www", "target": "1.2.3.4", "ttl_sec": 300})", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(LinodeDriverTest, CheckResponse_200_EmptyBody_ReturnsTrue) {
    // Linode returns 200 on success even with minimal body;
    // our implementation checks HTTP 200 first.
    LinodeDriver driver;
    HttpResponse resp{200, "", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(LinodeDriverTest, CheckResponse_Non200_WithErrorBody_ReturnsFalse) {
    LinodeDriver driver;
    HttpResponse resp{400, R"({"errors": [{"field": "type", "reason": "Invalid record type"}]})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(LinodeDriverTest, CheckResponse_Non200_WithMultipleErrors_ReturnsFalse) {
    LinodeDriver driver;
    HttpResponse resp{400, R"({
        "errors": [
            {"field": "name", "reason": "Invalid name"},
            {"field": "target", "reason": "Invalid target"}
        ]
    })", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(LinodeDriverTest, CheckResponse_Non200_WithEmptyFieldError_ReturnsFalse) {
    LinodeDriver driver;
    HttpResponse resp{400, R"({"errors": [{"field": "", "reason": "Rate limit exceeded"}]})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(LinodeDriverTest, CheckResponse_Non200_UnparseableBody_ReturnsFalse) {
    LinodeDriver driver;
    HttpResponse resp{400, "not-json", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(LinodeDriverTest, CheckResponse_Non200_EmptyBody_ReturnsFalse) {
    LinodeDriver driver;
    HttpResponse resp{500, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(LinodeDriverTest, FactoryCreateDestroy) { test_factory_create_destroy(); }
TEST(LinodeDriverTest, FactoryMagic) { test_factory_magic(); }
TEST(LinodeDriverTest, FactoryBuildId) { test_factory_build_id(); }
TEST(LinodeDriverTest, FactoryCompilerIdHash) { test_factory_compiler_id_hash(); }

TEST(LinodeDriverTest, CheckResponse_Non200_ErrorFieldEmpty_ReturnsFalse) {
    LinodeDriver driver;
    HttpResponse resp{400, R"({"errors": [{"field": "type", "reason": "Invalid"}]})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}
TEST(LinodeDriverTest, CheckResponse_Non200_NoErrorsKey_ReturnsFalse) {
    LinodeDriver driver;
    HttpResponse resp{400, R"({"other": "data"})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}
