//
// Port mocks — GoogleMock doubles for the retained application ports
// (src/application/ports/), used by the diagnostics and validator tests.
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_MOCK_PORTS_H
#define YADDNSC_TEST_MOCKS_MOCK_PORTS_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <expected>
#include <gmock/gmock.h>

#include "application/ports/driver_catalog.h"
#include "application/ports/network_interfaces.h"

class MockNetworkInterfaces final : public app::NetworkInterfacesPort {
public:
    MOCK_METHOD(std::vector<std::string>, names, (), (const, override));
    MOCK_METHOD((std::optional<std::vector<domain::InetAddress>>), addresses, (const std::string& name),
                (const, override));
};

class MockDriverCatalogPort final : public app::DriverCatalogPort {
public:
    MOCK_METHOD(std::vector<std::string>, loaded_drivers, (), (const, override));
    MOCK_METHOD((std::expected<app::DriverDescription, domain::DriverError>), describe, (std::string_view name),
                (const, override));
};

#endif  // YADDNSC_TEST_MOCKS_MOCK_PORTS_H
