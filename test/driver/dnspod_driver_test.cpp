//
// Unit tests for DNSPodDriver (driver/dnspod/)
//
// Verifies:
//   - get_detail() returns expected metadata.
//   - generate_request() builds correct DNSPod API URL (CN).
//   - generate_request() builds correct DNSPod API URL (global).
//   - generate_request() produces form-encoded body with all expected fields.
//   - generate_request() uses "默认" record_line by default for CN.
//   - generate_request() uses "default" record_line for global.
//   - generate_request() with missing config throws ParamParseException.
//   - check_response() returns true for status code "1" with record.
//   - check_response() returns false for other status codes.
//   - check_response() returns false when status is missing.
//   - check_response() returns false for unparseable response.
// =============================================================================

#include <gtest/gtest.h>

#include "dnspod.h"
#include "config.hpp"
#include "response.hpp"
#include "factory_test_helpers.h"

TEST(DNSPodDriverTest, GetDetail_ReturnsExpectedMetadata) {
    DNSPodDriver driver;
    auto detail = driver.get_detail();
    EXPECT_EQ(detail.name, "dnspod");
    EXPECT_EQ(detail.description, "Updates DNS records via the DNSPod API");
    EXPECT_EQ(detail.author, "Kotarou");
    EXPECT_EQ(detail.version, "2.0.0");
}

TEST(DNSPodDriverTest, GenerateRequest_DefaultEndpointCn) {
    DNSPodDriver driver;
    DriverConfig config = R"({
        "domain_id": "dom123",
        "record_id": "rec456",
        "login_token": "token123",
        "record_line_id": "0",
        "global": false
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);
    EXPECT_EQ(result.url, "https://dnsapi.cn/Record.Ddns");
    EXPECT_EQ(result.request.method, DriverHttpMethod::POST);
    EXPECT_EQ(result.request.content_type, "application/x-www-form-urlencoded");
}

TEST(DNSPodDriverTest, GenerateRequest_GlobalEndpoint) {
    DNSPodDriver driver;
    DriverConfig config = R"({
        "domain_id": "dom123",
        "record_id": "rec456",
        "login_token": "token123",
        "global": true,
        "record_line_id": "0"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);
    EXPECT_EQ(result.url, "https://api.dnspod.com/Record.Ddns");
}

TEST(DNSPodDriverTest, GenerateRequest_BodyContainsRequiredFields) {
    DNSPodDriver driver;
    DriverConfig config = R"({
        "domain_id": "dom123",
        "record_id": "rec456",
        "login_token": "token123",
        "record_line_id": "0",
        "global": false
    })";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "AAAA",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };

    auto result = driver.generate_request(config, ctx);
    ASSERT_TRUE(result.request.body.has_value());
    auto &body = result.request.body.value();

    EXPECT_TRUE(body.find("login_token=token123") != std::string::npos);
    EXPECT_TRUE(body.find("domain_id=dom123") != std::string::npos);
    EXPECT_TRUE(body.find("record_id=rec456") != std::string::npos);
    EXPECT_TRUE(body.find("sub_domain=@") != std::string::npos);
    EXPECT_TRUE(body.find("record_type=AAAA") != std::string::npos);
    EXPECT_TRUE(body.find("value=10.0.0.1") != std::string::npos);
    EXPECT_TRUE(body.find("format=json") != std::string::npos);
}

TEST(DNSPodDriverTest, GenerateRequest_DefaultRecordLine_Cn) {
    DNSPodDriver driver;
    DriverConfig config = R"({
        "domain_id": "dom123",
        "record_id": "rec456",
        "login_token": "token123",
        "record_line_id": "0",
        "global": false
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);
    ASSERT_TRUE(result.request.body.has_value());
    // CN default record_line should be "默认"
    EXPECT_TRUE(result.request.body.value().find("record_line=%E9%BB%98%E8%AE%A4") != std::string::npos ||
                result.request.body.value().find("record_line=默认") != std::string::npos);
}

TEST(DNSPodDriverTest, GenerateRequest_DefaultRecordLine_Global) {
    DNSPodDriver driver;
    DriverConfig config = R"({
        "domain_id": "dom123",
        "record_id": "rec456",
        "login_token": "token123",
        "global": true,
        "record_line_id": "0"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);
    ASSERT_TRUE(result.request.body.has_value());
    EXPECT_TRUE(result.request.body.value().find("record_line=default") != std::string::npos);
}

TEST(DNSPodDriverTest, GenerateRequest_CustomRecordLine) {
    DNSPodDriver driver;
    DriverConfig config = R"({
        "domain_id": "dom123",
        "record_id": "rec456",
        "login_token": "token123",
        "record_line": "unicom",
        "record_line_id": "0",
        "global": false
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);
    ASSERT_TRUE(result.request.body.has_value());
    EXPECT_TRUE(result.request.body.value().find("record_line=unicom") != std::string::npos);
}

TEST(DNSPodDriverTest, GenerateRequest_MissingDomainId_ThrowsParamParseException) {
    DNSPodDriver driver;
    DriverConfig config = R"({"record_id": "rec456", "login_token": "token123"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(DNSPodDriverTest, GenerateRequest_MissingLoginToken_ThrowsParamParseException) {
    DNSPodDriver driver;
    DriverConfig config = R"({"domain_id": "dom123", "record_id": "rec456"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(DNSPodDriverTest, CheckResponse_StatusCode1WithRecord_ReturnsTrue) {
    DNSPodDriver driver;
    HttpResponse resp{200, R"({
        "status": {"code": "1", "message": "Action completed successfully", "created_at": "2024-01-01 00:00:00"},
        "record": {"id": 123, "name": "www.example.com", "value": "1.2.3.4"}
    })", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(DNSPodDriverTest, CheckResponse_StatusCode1NoRecord_ReturnsTrue) {
    DNSPodDriver driver;
    HttpResponse resp{200, R"({
        "status": {"code": "1", "message": "Action completed successfully", "created_at": "2024-01-01 00:00:00"}
    })", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(DNSPodDriverTest, CheckResponse_ErrorStatusCode_ReturnsFalse) {
    DNSPodDriver driver;
    HttpResponse resp{200, R"({
        "status": {"code": "-1", "message": "Login fails", "created_at": "2024-01-01 00:00:00"}
    })", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(DNSPodDriverTest, CheckResponse_MissingStatus_ReturnsFalse) {
    DNSPodDriver driver;
    HttpResponse resp{200, R"({"record": {"id": 123, "name": "www", "value": "1.2.3.4"}})", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(DNSPodDriverTest, CheckResponse_UnparseableBody_ReturnsFalse) {
    DNSPodDriver driver;
    HttpResponse resp{200, "not-json", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(DNSPodDriverTest, CheckResponse_EmptyBody_ReturnsFalse) {
    DNSPodDriver driver;
    HttpResponse resp{200, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(DNSPodDriverTest, FactoryCreateDestroy) { test_factory_create_destroy(); }
TEST(DNSPodDriverTest, FactoryMagic) { test_factory_magic(); }
TEST(DNSPodDriverTest, FactoryBuildId) { test_factory_build_id(); }
TEST(DNSPodDriverTest, FactoryCompilerIdHash) { test_factory_compiler_id_hash(); }
