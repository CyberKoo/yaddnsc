//
// Unit tests for Route53Driver (driver/route53/)
//
// Verifies:
//   - get_detail() returns expected metadata.
//   - generate_request() builds correct Route 53 API URL.
//   - generate_request() sets required SigV4 headers.
//   - generate_request() produces valid XML request body with UPSERT action.
//   - generate_request() uses configurable TTL.
//   - generate_request() ensures FQDN has trailing dot.
//   - generate_request() with missing config throws ParamParseException.
//   - check_response() returns true for XML with PENDING status.
//   - check_response() returns true for XML with INSYNC status.
//   - check_response() returns false for non-200 status.
//   - check_response() returns false for malformed XML.
//   - check_response() returns false for empty body.
// =============================================================================

#include <gtest/gtest.h>

#include "route53.h"
#include "config.hpp"
#include "factory_test_helpers.h"

namespace {

/// Build a Route 53 ChangeResourceRecordSets success response XML.
std::string make_success_xml(std::string_view status) {
    return fmt::format(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<ChangeResourceRecordSetsResponse xmlns="https://route53.amazonaws.com/doc/2013-04-01/">
  <ChangeInfo>
    <Id>/change/C2682N5HXP0BZ4</Id>
    <Status>{}</Status>
    <SubmittedAt>2024-01-01T00:00:00Z</SubmittedAt>
  </ChangeInfo>
</ChangeResourceRecordSetsResponse>)", status);
}

/// Build a Route 53 error response XML.
std::string make_error_xml(std::string_view code, std::string_view message) {
    return fmt::format(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<ErrorResponse xmlns="https://route53.amazonaws.com/doc/2013-04-01/">
  <Error>
    <Type>Sender</Type>
    <Code>{}</Code>
    <Message>{}</Message>
  </Error>
  <RequestId>req123</RequestId>
</ErrorResponse>)", code, message);
}

} // anonymous namespace

TEST(Route53DriverTest, GetDetail_ReturnsExpectedMetadata) {
    Route53Driver driver;
    auto detail = driver.get_detail();
    EXPECT_EQ(detail.name, "route53");
    EXPECT_EQ(detail.description, "Updates DNS records via the AWS Route 53 API");
    EXPECT_EQ(detail.author, "Kotarou");
    EXPECT_EQ(detail.version, "1.0.0");
}

TEST(Route53DriverTest, GenerateRequest_BasicARecord) {
    Route53Driver driver;
    DriverConfig config = R"({
        "access_key_id": "AKID123",
        "secret_access_key": "secret456",
        "hosted_zone_id": "Z3M79L5CQABCDE",
        "region": "us-east-1",
        "record_name": "www.example.com"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);

    // Check URL
    EXPECT_EQ(result.url,
              "https://route53.amazonaws.com/2013-04-01/hostedzone/Z3M79L5CQABCDE/rrset");

    // Check method and content type
    EXPECT_EQ(result.request.method, DriverHttpMethod::POST);
    EXPECT_EQ(result.request.content_type, "application/xml");

    // Check SigV4 headers are present (use find() — headers is multimap)
    {
        auto it = result.request.headers.find("Host");
        ASSERT_NE(it, result.request.headers.end());
        EXPECT_EQ(it->second, "route53.amazonaws.com");
    }
    EXPECT_NE(result.request.headers.find("X-Amz-Date"), result.request.headers.end());
    EXPECT_NE(result.request.headers.find("X-Amz-Content-SHA256"), result.request.headers.end());
    EXPECT_NE(result.request.headers.find("Authorization"), result.request.headers.end());

    // Verify Authorization header starts with AWS4-HMAC-SHA256
    auto auth_it = result.request.headers.find("Authorization");
    ASSERT_NE(auth_it, result.request.headers.end());
    auto &auth = auth_it->second;
    EXPECT_TRUE(auth.starts_with("AWS4-HMAC-SHA256"));
    EXPECT_TRUE(auth.find("Credential=AKID123") != std::string::npos);
    EXPECT_TRUE(auth.find("us-east-1/route53/aws4_request") != std::string::npos);

    // Check body contains XML
    ASSERT_TRUE(result.request.body.has_value());
    auto &body = result.request.body.value();
    EXPECT_TRUE(body.find("ChangeResourceRecordSetsRequest") != std::string::npos);
    EXPECT_TRUE(body.find("UPSERT") != std::string::npos);
    EXPECT_TRUE(body.find("www.example.com.") != std::string::npos);  // trailing dot
    EXPECT_TRUE(body.find("A") != std::string::npos);
    EXPECT_TRUE(body.find("1.2.3.4") != std::string::npos);
    EXPECT_TRUE(body.find("<TTL>300</TTL>") != std::string::npos);  // default TTL
}

TEST(Route53DriverTest, GenerateRequest_FqdnWithoutDot_AddsTrailingDot) {
    Route53Driver driver;
    DriverConfig config = R"({
        "access_key_id": "AKID123",
        "secret_access_key": "secret456",
        "hosted_zone_id": "ZONE1",
        "region": "us-west-2",
        "record_name": "test.example.com"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "AAAA",
        .domain = "example.com", .subdomain = "test", .fqdn = "test.example.com"
    };

    auto result = driver.generate_request(config, ctx);
    ASSERT_TRUE(result.request.body.has_value());
    // Route 53 requires FQDN with trailing dot
    EXPECT_TRUE(result.request.body.value().find("test.example.com.") != std::string::npos);
}

TEST(Route53DriverTest, GenerateRequest_WithCustomTtl) {
    Route53Driver driver;
    DriverConfig config = R"({
        "access_key_id": "AKID123",
        "secret_access_key": "secret456",
        "hosted_zone_id": "ZONE1",
        "region": "eu-west-1",
        "record_name": "www.example.com",
        "ttl": 60
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);
    ASSERT_TRUE(result.request.body.has_value());
    EXPECT_TRUE(result.request.body.value().find("<TTL>60</TTL>") != std::string::npos);
}

TEST(Route53DriverTest, GenerateRequest_MissingAccessKey_ThrowsParamParseException) {
    Route53Driver driver;
    DriverConfig config = R"({
        "secret_access_key": "secret456",
        "hosted_zone_id": "ZONE1",
        "region": "us-east-1",
        "record_name": "test"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(Route53DriverTest, GenerateRequest_MissingHostedZoneId_ThrowsParamParseException) {
    Route53Driver driver;
    DriverConfig config = R"({
        "access_key_id": "AKID123",
        "secret_access_key": "secret456",
        "region": "us-east-1",
        "record_name": "test"
    })";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(Route53DriverTest, CheckResponse_PendingStatus_ReturnsTrue) {
    Route53Driver driver;
    HttpResponse resp{200, make_success_xml("PENDING"), {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(Route53DriverTest, CheckResponse_InsyncStatus_ReturnsTrue) {
    Route53Driver driver;
    HttpResponse resp{200, make_success_xml("INSYNC"), {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(Route53DriverTest, CheckResponse_UnexpectedStatus_ReturnsFalse) {
    Route53Driver driver;
    HttpResponse resp{200, make_success_xml("FAILED"), {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(Route53DriverTest, CheckResponse_MissingStatus_ReturnsFalse) {
    Route53Driver driver;
    auto xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<ChangeResourceRecordSetsResponse xmlns="https://route53.amazonaws.com/doc/2013-04-01/">
  <ChangeInfo>
    <Id>/change/C2682N5HXP0BZ4</Id>
    <SubmittedAt>2024-01-01T00:00:00Z</SubmittedAt>
  </ChangeInfo>
</ChangeResourceRecordSetsResponse>)";
    HttpResponse resp{200, xml, {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(Route53DriverTest, CheckResponse_Non200_WithErrorXml_ReturnsFalse) {
    Route53Driver driver;
    HttpResponse resp{400, make_error_xml("InvalidChangeBatch", "RRset with name www.example.com. and type A is not supported"), {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(Route53DriverTest, CheckResponse_Non200_WithMultipleErrors_ReturnsFalse) {
    Route53Driver driver;
    auto xml = fmt::format(
        R"(<?xml version="1.0" encoding="UTF-8"?>
<ErrorResponse xmlns="https://route53.amazonaws.com/doc/2013-04-01/">
  <Error>
    <Type>Sender</Type>
    <Code>AccessDenied</Code>
    <Message>User is not authorized</Message>
  </Error>
  <Error>
    <Type>Sender</Type>
    <Code>Throttling</Code>
    <Message>Rate exceeded</Message>
  </Error>
  <RequestId>req456</RequestId>
</ErrorResponse>)");
    HttpResponse resp{403, xml, {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(Route53DriverTest, CheckResponse_Non200_UnparseableBody_ReturnsFalse) {
    Route53Driver driver;
    HttpResponse resp{400, "not xml", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(Route53DriverTest, CheckResponse_Non200_EmptyBody_ReturnsFalse) {
    Route53Driver driver;
    HttpResponse resp{500, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(Route53DriverTest, CheckResponse_MalformedSuccessXml_ReturnsFalse) {
    Route53Driver driver;
    HttpResponse resp{200, "not valid xml at all", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(Route53DriverTest, FactoryCreateDestroy) { test_factory_create_destroy(); }
TEST(Route53DriverTest, FactoryMagic) { test_factory_magic(); }
TEST(Route53DriverTest, FactoryBuildId) { test_factory_build_id(); }
TEST(Route53DriverTest, FactoryCompilerIdHash) { test_factory_compiler_id_hash(); }
