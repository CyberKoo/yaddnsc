//
// MockDriver — GoogleMock-based mock for the Driver interface.
//
// Provides configurable expectations for all Driver virtual methods so that
// the scheduler, dispatcher, and other components can be tested without a
// real driver plugin (.so file).
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_MOCK_DRIVER_H
#define YADDNSC_TEST_MOCKS_MOCK_DRIVER_H

#include <string_view>

#include <gmock/gmock.h>

#include "interface/driver.h"

class MockDriver : public Driver {
public:
    MOCK_METHOD(DriverRequestContext, generate_request,
                (const DriverConfig& config, const DriverUpdateParams& ctx), (const, override));

    MOCK_METHOD(bool, check_response, (const HttpResponse& response), (const, override));

    MOCK_METHOD(DriverDetail, get_detail, (), (const, noexcept, override));

    MOCK_METHOD(AbiVersion, get_abi_version, (), (const, noexcept, override));

    // Default execute models BaseDriver: generate_request -> http.exchange ->
    // check_response. Tests that only set expectations on check_response/exchange
    // get realistic behaviour for free; tests that need custom execute logic can
    // override this with ON_CALL/EXPECT_CALL.
    bool execute(const DriverConfig& config, const DriverUpdateParams& ctx, HttpClient& http) const override {
        const auto [url, request] = generate_request(config, ctx);
        const auto response = http.exchange(url, request);
        if (!response) {
            return false;
        }
        return check_response(*response);
    }
};

// ── Helper: create a DriverDetail with the given name ─────────────────────────
inline DriverDetail make_driver_detail(std::string_view name,
                                       std::string_view description = "Mock driver",
                                       std::string_view author = "test",
                                       std::string_view version = "1.0.0") {
    return {name, description, author, version};
}

// ── Helper: create a default AbiVersion ──────────────────────────────────────
inline AbiVersion make_abi_version(std::uint16_t major = 1,
                                   std::uint16_t minor = 0,
                                   std::uint16_t patch = 0) {
    return {major, minor, patch};
}

#endif // YADDNSC_TEST_MOCKS_MOCK_DRIVER_H
