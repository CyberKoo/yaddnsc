//
// Unit tests for NamecheapDriver (driver/namecheap/)
//
// Verifies:
//   - get_detail() returns expected metadata.
//   - generate_request() builds correct Namecheap DDNS API URL.
//   - generate_request() rejects AAAA records with ParamParseException.
//   - generate_request() handles bare-domain (apex) records correctly.
//   - generate_request() with missing config throws ParamParseException.
//   - check_response() returns true for XML with ErrCount=0 and <IP> element.
//   - check_response() returns false for XML with ErrCount>0.
//   - check_response() returns false for malformed XML.
//   - check_response() returns false for empty body.
// =============================================================================

#include <gtest/gtest.h>

#include "namecheap.h"
#include "config.hpp"
#include "factory_test_helpers.h"

namespace {

/// Build a Namecheap API success XML response with the given IP.
std::string make_success_xml(std::string_view ip) {
    return fmt::format(
        R"(<?xml version="1.0"?>
<interface-response>
  <Command>NAMEcheap.dynamicdns.update</Command>
  <ErrCount>0</ErrCount>
  <Done>true</Done>
  <IP>{}</IP>
</interface-response>)", ip);
}

/// Build a Namecheap API error XML response with the given error message.
std::string make_error_xml(std::string_view err_msg) {
    return fmt::format(
        R"(<?xml version="1.0"?>
<interface-response>
  <Command>NAMEcheap.dynamicdns.update</Command>
  <ErrCount>1</ErrCount>
  <Done>true</Done>
  <errors>
    <error>{}</error>
  </errors>
</interface-response>)", err_msg);
}

} // anonymous namespace

TEST(NamecheapDriverTest, GetDetail_ReturnsExpectedMetadata) {
    NamecheapDriver driver;
    auto detail = driver.get_detail();
    EXPECT_EQ(detail.name, "namecheap");
    EXPECT_EQ(detail.description, "Updates DNS records via the Namecheap Dynamic DNS API");
    EXPECT_EQ(detail.author, "Kotarou");
    EXPECT_EQ(detail.version, "1.0.0");
}

TEST(NamecheapDriverTest, GenerateRequest_BasicARecord) {
    NamecheapDriver driver;
    DriverConfig config = R"({"password": "my-pass"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    auto result = driver.generate_request(config, ctx);

    // Check URL
    EXPECT_EQ(result.url,
              "https://dynamicdns.park-your-domain.com/update?host=www&domain=example.com&password=my-pass&ip=1.2.3.4");
    EXPECT_EQ(result.request.method, DriverHttpMethod::GET);
    EXPECT_FALSE(result.request.body.has_value());
}

TEST(NamecheapDriverTest, GenerateRequest_ApexDomain) {
    NamecheapDriver driver;
    DriverConfig config = R"({"password": "my-pass"})";
    DriverUpdateParams ctx{
        .ip_addr = "10.0.0.1", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };

    auto result = driver.generate_request(config, ctx);
    EXPECT_TRUE(result.url.find("host=@") != std::string::npos)
        << "Apex domain should use @ as host, got URL: " << result.url;
}

TEST(NamecheapDriverTest, GenerateRequest_AAAARecord_ThrowsParamParseException) {
    NamecheapDriver driver;
    DriverConfig config = R"({"password": "my-pass"})";
    DriverUpdateParams ctx{
        .ip_addr = "::1", .rd_type = "AAAA",
        .domain = "example.com", .subdomain = "www", .fqdn = "www.example.com"
    };

    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(NamecheapDriverTest, GenerateRequest_MissingPassword_ThrowsParamParseException) {
    NamecheapDriver driver;
    DriverConfig config = R"({"not_password": "value"})";
    DriverUpdateParams ctx{
        .ip_addr = "1.2.3.4", .rd_type = "A",
        .domain = "example.com", .subdomain = "@", .fqdn = "example.com"
    };
    EXPECT_THROW({ driver.generate_request(config, ctx); }, ParamParseException);
}

TEST(NamecheapDriverTest, CheckResponse_Success_ReturnsTrue) {
    NamecheapDriver driver;
    auto xml = make_success_xml("1.2.3.4");
    HttpResponse resp{200, xml, {}};
    EXPECT_TRUE(driver.check_response(resp));
}

TEST(NamecheapDriverTest, CheckResponse_Error_ReturnsFalse) {
    NamecheapDriver driver;
    auto xml = make_error_xml("Domain name not found");
    HttpResponse resp{200, xml, {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(NamecheapDriverTest, CheckResponse_MultipleErrors_ReturnsFalse) {
    NamecheapDriver driver;
    auto xml = fmt::format(
        R"(<?xml version="1.0"?>
<interface-response>
  <ErrCount>2</ErrCount>
  <errors>
    <error>First error</error>
    <error>Second error</error>
  </errors>
</interface-response>)");
    HttpResponse resp{200, xml, {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(NamecheapDriverTest, CheckResponse_MalformedXml_ReturnsFalse) {
    NamecheapDriver driver;
    HttpResponse resp{200, "not xml", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(NamecheapDriverTest, CheckResponse_MissingErrCount_ReturnsFalse) {
    NamecheapDriver driver;
    auto xml = R"(<?xml version="1.0"?><interface-response><Done>true</Done></interface-response>)";
    HttpResponse resp{200, xml, {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(NamecheapDriverTest, CheckResponse_EmptyBody_ReturnsFalse) {
    NamecheapDriver driver;
    HttpResponse resp{200, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(NamecheapDriverTest, CheckResponse_Non200_ReturnsFalse) {
    NamecheapDriver driver;
    HttpResponse resp{500, "", {}};
    EXPECT_FALSE(driver.check_response(resp));
}

TEST(NamecheapDriverTest, FactoryCreateDestroy) { test_factory_create_destroy(); }
TEST(NamecheapDriverTest, FactoryMagic) { test_factory_magic(); }
TEST(NamecheapDriverTest, FactoryBuildId) { test_factory_build_id(); }
TEST(NamecheapDriverTest, FactoryCompilerIdHash) { test_factory_compiler_id_hash(); }
