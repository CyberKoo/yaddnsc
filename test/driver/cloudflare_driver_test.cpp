//
// Unit tests for CloudflareDriver (driver/cloudflare/)
//
// Verifies:
//   - get_detail() returns expected metadata.
//   - generate_request() builds correct Cloudflare API URL.
//   - generate_request() sets Bearer authorization header.
//   - generate_request() produces valid JSON request body.
//   - generate_request() uses config values for zone/record IDs.
//   - generate_request() with missing config fields throws ParamParseException.
//   - check_response() returns true for success=true with result.
//   - check_response() returns false for success=false with errors.
//   - check_response() returns false for unparseable response.
// =============================================================================

#include <gtest/gtest.h>

#include "cloudflare.h"
#include "config.hpp"
#include "response.hpp"
#include "factory_test_helpers.h"

// ── Helper: build a minimal success response ─────────────────────────────────
std::string make_success_response(std::string_view type, std::string_view name,
                                  std::string_view content, int ttl, bool proxied) {
    return fmt::format(R"({{"success":true,"errors":[],"messages":[],"result":{{"id":"rec123","name":"{}","type":"{}","content":"{}","ttl":{},"proxied":{},"proxiable":false}}}})",
                       name, type, content, ttl, proxied ? "true" : "false");
}

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(CloudflareDriverTest, GetDetail_ReturnsExpectedMetadata) {
    CloudflareDriver driver;
    auto detail = driver.get_detail();
    EXPECT_EQ(detail.name, "cloudflare");
    EXPECT_EQ(detail.description, "Updates DNS records via the Cloudflare API");
    EXPECT_EQ(detail.author, "Kotarou");
    EXPECT_EQ(detail.version, "2.0.0");
}

TEST(CloudflareDriverTest, GenerateRequest_BasicARecord) {
    CloudflareDriver driver;
    DriverConfig config = R"({
        "zone_id": "myzone",
        "record_id": "rec123",
        "token": "mytoken"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);

    // Check URL
    EXPECT_EQ(result.url, "https://api.cloudflare.com/client/v4/zones/myzone/dns_records/rec123");

    // Check method and content type
    EXPECT_EQ(result.request.method, DriverHttpMethod::PUT);
    EXPECT_EQ(result.request.content_type, "application/json");

    // Check auth header
    auto auth_it = result.request.headers.find("Authorization");
    ASSERT_NE(auth_it, result.request.headers.end());
    EXPECT_EQ(auth_it->second, "Bearer mytoken");

    // Check request body exists and contains expected fields
    ASSERT_TRUE(result.request.body.has_value());
    auto &body = result.request.body.value();
    EXPECT_TRUE(body.find(R"("type":"A")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("content":"1.2.3.4")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("name":"www")") != std::string::npos);
}

TEST(CloudflareDriverTest, GenerateRequest_WithTtlAndProxied) {
    CloudflareDriver driver;
    DriverConfig config = R"({
        "zone_id": "z1",
        "record_id": "r1",
        "token": "t1",
        "ttl": 120,
        "proxied": true
    })";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "AAAA",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };

    auto result = driver.generate_request(config, ctx);

    ASSERT_TRUE(result.request.body.has_value());
    auto &body = result.request.body.value();
    EXPECT_TRUE(body.find(R"("type":"AAAA")") != std::string::npos);
    EXPECT_TRUE(body.find(R"("ttl":120)") != std::string::npos);
    EXPECT_TRUE(body.find(R"("proxied":true)") != std::string::npos);
    EXPECT_TRUE(body.find(R"("content":"10.0.0.1")") != std::string::npos);
}

TEST(CloudflareDriverTest, GenerateRequest_MissingZoneId_ThrowsParamParseException) {
    CloudflareDriver driver;
    DriverConfig config = R"({"record_id": "r1", "token": "t1"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(CloudflareDriverTest, GenerateRequest_MissingToken_ThrowsParamParseException) {
    CloudflareDriver driver;
    DriverConfig config = R"({"zone_id": "z1", "record_id": "r1"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(CloudflareDriverTest, CheckResponse_Success_ReturnsTrue) {
    CloudflareDriver driver;
    auto body = make_success_response("A", "www.example.com", "1.2.3.4", 120, false);
    HttpResponse resp{200, body, {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(CloudflareDriverTest, CheckResponse_SuccessWithProxied_ReturnsTrue) {
    CloudflareDriver driver;
    auto body = make_success_response("A", "www.example.com", "1.2.3.4", 30, true);
    HttpResponse resp{200, body, {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(CloudflareDriverTest, CheckResponse_SuccessWithoutResult_ReturnsTrue) {
    // Cloudflare can return success=true with no result field for certain operations.
    CloudflareDriver driver;
    HttpResponse resp{200, R"({"success":true,"errors":[],"messages":[]})", {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(CloudflareDriverTest, CheckResponse_ErrorWithSource_ReturnsFalse) {
    CloudflareDriver driver;
    HttpResponse resp{400, R"({
        "success": false,
        "errors": [{"code": 7003, "message": "Could not find zone", "source": {"pointer": "/zone_id"}}],
        "messages": []
    })", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(CloudflareDriverTest, CheckResponse_ErrorWithoutSource_ReturnsFalse) {
    CloudflareDriver driver;
    HttpResponse resp{400, R"({
        "success": false,
        "errors": [{"code": 9003, "message": "Record not found"}],
        "messages": []
    })", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(CloudflareDriverTest, CheckResponse_UnparseableBody_ReturnsFalse) {
    CloudflareDriver driver;
    HttpResponse resp{200, "not-json-at-all", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(CloudflareDriverTest, CheckResponse_EmptyBody_ReturnsFalse) {
    CloudflareDriver driver;
    HttpResponse resp{200, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(CloudflareDriverTest, CheckResponse_MultipleErrors_ReturnsFalse) {
    CloudflareDriver driver;
    HttpResponse resp{400, R"({
        "success": false,
        "errors": [
            {"code": 1001, "message": "First error"},
            {"code": 1002, "message": "Second error"}
        ],
        "messages": []
    })", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(CloudflareDriverTest, FactoryCreateDestroy) { test_factory_create_destroy(); }
TEST(CloudflareDriverTest, FactoryMagic) { test_factory_magic(); }
TEST(CloudflareDriverTest, FactoryBuildId) { test_factory_build_id(); }
TEST(CloudflareDriverTest, FactoryCompilerIdHash) { test_factory_compiler_id_hash(); }
