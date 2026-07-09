//
// MockDriverManager — GoogleMock-based mock for the DriverManagerBase interface.
//
// Provides configurable expectations for all DriverManagerBase methods so
// that ConfigValidator and other components can be tested without dlopen.
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_MOCK_DRIVER_MANAGER_H
#define YADDNSC_TEST_MOCKS_MOCK_DRIVER_MANAGER_H

#include <string>
#include <vector>
#include <string_view>

#include <gmock/gmock.h>

#include "core/driver_manager.h"

class MockDriverManager : public DriverManagerBase {
public:
    MOCK_METHOD(void, load_driver, (const std::string& path), (const, override));
    MOCK_METHOD(void, unload_driver, (const std::string& name), (override));
    MOCK_METHOD(std::vector<std::string_view>, get_loaded_drivers, (), (const, override));
    MOCK_METHOD(const Driver&, get_driver, (const std::string& name), (const, override));
};

#endif // YADDNSC_TEST_MOCKS_MOCK_DRIVER_MANAGER_H
