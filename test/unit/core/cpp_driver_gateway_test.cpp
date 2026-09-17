//
// Contract tests for src/core/cpp_driver_gateway.cpp — CppDriverGateway.
//
// Locks the translation from the legacy C++ Driver contract (bool return +
// exceptions) to the DriverGateway error-value contract:
//   - execute() == true                → success; command fields forwarded;
//                                        one fresh HttpClient per update
//   - execute() == false (upstream)    → UPDATE_FAILED
//   - execute() == false (HTTP error)  → UPDATE_FAILED
//   - driver not loaded                → NOT_FOUND, message from the exception
//   - exception from execute()         → UNKNOWN, message from the exception
//   - non-standard throw               → UNKNOWN
// =============================================================================

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "core/cpp_driver_gateway.h"
#include "exception/driver_not_found.h"

#include "mocks/mock_driver.h"
#include "mocks/mock_driver_manager.h"
#include "mocks/mock_http_client.h"

namespace {

using ::testing::_;
using ::testing::Return;
using ::testing::ReturnRef;

[[nodiscard]] DriverUpdateCommand make_command() {
    return DriverUpdateCommand{
        .driver_param = R"({"token":"fake"})",
        .ip_addr = "198.51.100.1",
        .rd_type = "A",
        .domain = "example.com",
        .subdomain = "www",
        .fqdn = "www.example.com",
    };
}

[[nodiscard]] CppDriverGateway make_gateway(const DriverManagerBase &manager, HttpClientFactory factory) {
    return CppDriverGateway(manager, std::move(factory));
}

} // namespace

TEST(CppDriverGateway, SuccessForwardsCommandAndUsesFreshClient) {
    MockDriverManager manager;
    MockDriver driver;
    auto http = std::make_unique<MockHttpClient>();
    auto *http_ptr = http.get();

    int factory_calls = 0;
    auto gateway = make_gateway(manager, [&factory_calls, &http]() mutable {
        ++factory_calls;
        return std::move(http);
    });

    EXPECT_CALL(manager, get_driver("simple")).WillOnce(ReturnRef(driver));
    EXPECT_CALL(*http_ptr, exchange("https://api.example.com/update", _))
        .WillOnce(Return(net::http::Response{200, "ok", {}}));
    EXPECT_CALL(driver, generate_request(R"({"token":"fake"})", _))
        .WillOnce([](const DriverConfig &, const DriverUpdateParams &ctx) {
            EXPECT_EQ(ctx.ip_addr, "198.51.100.1");
            EXPECT_EQ(ctx.rd_type, "A");
            EXPECT_EQ(ctx.domain, "example.com");
            EXPECT_EQ(ctx.subdomain, "www");
            EXPECT_EQ(ctx.fqdn, "www.example.com");
            return DriverRequestContext{.url = "https://api.example.com/update", .request = {}};
        });
    EXPECT_CALL(driver, check_response(_)).WillOnce(Return(true));

    const auto result = gateway.update("simple", make_command());

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(factory_calls, 1);
}

TEST(CppDriverGateway, UpstreamRejectionBecomesUpdateFailed) {
    MockDriverManager manager;
    MockDriver driver;
    auto http = std::make_unique<MockHttpClient>();
    auto *http_ptr = http.get();

    EXPECT_CALL(manager, get_driver("simple")).WillOnce(ReturnRef(driver));
    EXPECT_CALL(driver, generate_request(_, _))
        .WillOnce(Return(DriverRequestContext{.url = "https://api.example.com/update", .request = {}}));
    EXPECT_CALL(*http_ptr, exchange(_, _)).WillOnce(Return(net::http::Response{200, "ok", {}}));
    EXPECT_CALL(driver, check_response(_)).WillOnce(Return(false));

    auto gateway = make_gateway(manager, [&http]() mutable { return std::move(http); });
    const auto result = gateway.update("simple", make_command());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UPDATE_FAILED);
}

TEST(CppDriverGateway, HttpErrorBecomesUpdateFailed) {
    MockDriverManager manager;
    MockDriver driver;
    auto http = std::make_unique<MockHttpClient>();
    auto *http_ptr = http.get();

    EXPECT_CALL(manager, get_driver("simple")).WillOnce(ReturnRef(driver));
    EXPECT_CALL(driver, generate_request(_, _))
        .WillOnce(Return(DriverRequestContext{.url = "https://api.example.com/update", .request = {}}));
    EXPECT_CALL(*http_ptr, exchange(_, _))
        .WillOnce(Return(std::unexpected(net::http::Error{net::http::ErrorCode::CONNECT_FAILED, "refused"})));
    // MockDriver::execute models BaseDriver: HTTP error → false, check_response
    // is never reached.
    EXPECT_CALL(driver, check_response(_)).Times(0);

    auto gateway = make_gateway(manager, [&http]() mutable { return std::move(http); });
    const auto result = gateway.update("simple", make_command());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UPDATE_FAILED);
}

TEST(CppDriverGateway, UnknownDriverBecomesNotFound) {
    MockDriverManager manager;
    EXPECT_CALL(manager, get_driver("ghost"))
        .WillOnce([](const std::string &name) -> const Driver & {
            throw DriverNotFoundException("Driver not found: " + name);
        });

    bool factory_called = false;
    auto gateway = make_gateway(manager, [&factory_called]() -> std::unique_ptr<HttpClient> {
        factory_called = true;
        return std::make_unique<MockHttpClient>();
    });

    const auto result = gateway.update("ghost", make_command());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::NOT_FOUND);
    EXPECT_EQ(result.error().message, "Driver not found: ghost");
    EXPECT_FALSE(factory_called) << "no HTTP client may be created when the driver is missing";
}

TEST(CppDriverGateway, DriverExceptionBecomesUnknown) {
    MockDriverManager manager;
    MockDriver driver;

    EXPECT_CALL(manager, get_driver("simple")).WillOnce(ReturnRef(driver));
    EXPECT_CALL(driver, generate_request(_, _))
        .WillOnce([](const DriverConfig &, const DriverUpdateParams &) -> DriverRequestContext {
            throw std::runtime_error("Driver configuration parse error: missing key");
        });

    auto gateway = make_gateway(manager, [] { return std::make_unique<MockHttpClient>(); });
    const auto result = gateway.update("simple", make_command());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
    EXPECT_EQ(result.error().message, "Driver configuration parse error: missing key");
}

TEST(CppDriverGateway, NonStandardThrowBecomesUnknown) {
    MockDriverManager manager;
    MockDriver driver;

    EXPECT_CALL(manager, get_driver("simple")).WillOnce(ReturnRef(driver));
    EXPECT_CALL(driver, generate_request(_, _))
        .WillOnce([](const DriverConfig &, const DriverUpdateParams &) -> DriverRequestContext {
            throw 42;
        });

    auto gateway = make_gateway(manager, [] { return std::make_unique<MockHttpClient>(); });
    const auto result = gateway.update("simple", make_command());

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, domain::DriverError::Code::UNKNOWN);
}
