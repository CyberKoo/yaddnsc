//
// Unit tests for SimpleDriver (driver/simple/)
//
// Verifies:
//   - get_detail() returns expected metadata.
//   - generate_request() substitutes URL template variables correctly.
//   - generate_request() with missing config throws ParamParseException.
//   - check_response() accepts 2xx with non-empty body.
//   - check_response() rejects 3xx/4xx/5xx status codes.
//   - check_response() rejects empty body.
// =============================================================================

#include <gtest/gtest.h>

#include "simple.h"

// Needed by DEFINE_DRIVER_FACTORY and CORE_LOG_* macros (from compiled .cpp).
// Also required by the driver's implementation.
#include "driver/factory.h"
#include "interface/core_logger.h"
#include "factory_test_helpers.h"

// ── Tests ──────────────────────────────────────────────────────────────────

TEST(SimpleDriverTest, GetDetail_ReturnsExpectedMetadata) {
    SimpleDriver driver;
    auto detail = driver.get_detail();
    EXPECT_EQ(detail.name, "simple");
    EXPECT_EQ(detail.description, "Generic HTTP driver with URL template substitution");
    EXPECT_EQ(detail.author, "Kotarou");
    EXPECT_EQ(detail.version, "2.0.0");
}

TEST(SimpleDriverTest, GenerateRequest_BasicUrlTemplate) {
    SimpleDriver driver;
    DriverConfig config = R"({
        "url": "https://dns.example.com/update?ip={ip_addr}&domain={fqdn}",
        "custom_param": "hello"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "192.168.1.1",
        .rd_type = "A",
        .domain = "example.com",
        .subdomain = "www",
        .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);
    EXPECT_EQ(result.url, "https://dns.example.com/update?ip=192.168.1.1&domain=www.example.com");
    EXPECT_EQ(result.request.method, DriverHttpMethod::GET);
    EXPECT_FALSE(result.request.body.has_value());
    EXPECT_TRUE(result.request.content_type.empty());
}

TEST(SimpleDriverTest, GenerateRequest_TemplateWithAllContextKeys) {
    SimpleDriver driver;
    DriverConfig config = R"({
        "url": "https://dns.example.com/{ip_addr}/{rd_type}/{domain}/{subdomain}/{fqdn}"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1",
        .rd_type = "AAAA",
        .domain = "test.org",
        .subdomain = "@",
        .fqdn = "test.org"
    };

    auto result = driver.generate_request(config, ctx);
    EXPECT_EQ(result.url, "https://dns.example.com/10.0.0.1/AAAA/test.org/@/test.org");
}

TEST(SimpleDriverTest, GenerateRequest_CustomParamsAreSubstituted) {
    SimpleDriver driver;
    DriverConfig config = R"({
        "url": "https://api.example.com/{token}?host={host}",
        "token": "my-secret-token",
        "host": "my-host"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A", .domain = "example.com",
        .subdomain = "@", .fqdn = "example.com"
    };

    auto result = driver.generate_request(config, ctx);
    EXPECT_EQ(result.url, "https://api.example.com/my-secret-token?host=my-host");
}

TEST(SimpleDriverTest, GenerateRequest_ConfigParamOverridesUrlToken) {
    // Custom config params are substituted before context params,
    // so config values can fill template slots.
    SimpleDriver driver;
    DriverConfig config = R"({
        "url": "https://{domain}/update?ip={ip_addr}",
        "domain": "override.example.com"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "A", .domain = "original.com",
        .subdomain = "@", .fqdn = "original.com"
    };

    auto result = driver.generate_request(config, ctx);
    // "domain" appears in config, so it's substituted BEFORE context substitution.
    // The config param "domain" is substituted first, then the context "domain".
    // But since "domain" was already in config, and {domain} was replaced with
    // "override.example.com", the context substitution for {domain} won't find it.
    EXPECT_EQ(result.url, "https://override.example.com/update?ip=10.0.0.1");
}

TEST(SimpleDriverTest, GenerateRequest_MissingUrl_ThrowsParamParseException) {
    SimpleDriver driver;
    DriverConfig config = R"({"not_url": "value"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A", .domain = "example.com",
        .subdomain = "@", .fqdn = "example.com"
    };

    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(SimpleDriverTest, GenerateRequest_EmptyConfig_ThrowsParamParseException) {
    SimpleDriver driver;
    DriverConfig config = "{}";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A", .domain = "example.com",
        .subdomain = "@", .fqdn = "example.com"
    };

    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(SimpleDriverTest, CheckResponse_2xxWithBody_ReturnsTrue) {
    SimpleDriver driver;
    HttpResponse resp{200, "update successful", {}};
    EXPECT_TRUE(driver.check_response(resp));

    HttpResponse resp204{204, "", {}};
    EXPECT_FALSE(driver.check_response(resp204));  // empty body → false
}

TEST(SimpleDriverTest, CheckResponse_2xxWithEmptyBody_ReturnsFalse) {
    SimpleDriver driver;
    HttpResponse resp{200, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(SimpleDriverTest, CheckResponse_3xx_ReturnsFalse) {
    SimpleDriver driver;
    HttpResponse resp{301, "redirect", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(SimpleDriverTest, CheckResponse_4xx_ReturnsFalse) {
    SimpleDriver driver;
    HttpResponse resp{404, "not found", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(SimpleDriverTest, CheckResponse_5xx_ReturnsFalse) {
    SimpleDriver driver;
    HttpResponse resp{500, "server error", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(SimpleDriverTest, CheckResponse_Non2xxWithEmptyBody_ReturnsFalse) {
    SimpleDriver driver;
    HttpResponse resp{400, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(SimpleDriverTest, FactoryCreateDestroy) { test_factory_create_destroy(); }
TEST(SimpleDriverTest, FactoryMagic) { test_factory_magic(); }
TEST(SimpleDriverTest, FactoryBuildId) { test_factory_build_id(); }
TEST(SimpleDriverTest, FactoryCompilerIdHash) { test_factory_compiler_id_hash(); }

TEST(SimpleDriverTest, GenerateRequest_NonStringConfigValue_IsSkipped) {
    SimpleDriver driver;
    // Non-string config values should be skipped during substitution.
    DriverConfig config = R"({"url": "https://example.com/{ip_addr}", "ttl": 300})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    auto result = driver.generate_request(config, ctx);
    EXPECT_EQ(result.url, "https://example.com/1.2.3.4");
}
