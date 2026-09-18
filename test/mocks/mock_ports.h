//
// Port mocks — GoogleMock doubles for the application ports
// (src/application/ports/), used by workflow and adapter tests.
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_MOCK_PORTS_H
#define YADDNSC_TEST_MOCKS_MOCK_PORTS_H

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>

#include "application/ports/dns_resolver.h"
#include "application/ports/driver_catalog.h"
#include "application/ports/driver_gateway.h"
#include "application/ports/ip_source.h"
#include "application/ports/network_interfaces.h"

class MockDnsResolverPort final : public DnsResolverPort {
public:
    MOCK_METHOD((std::expected<std::vector<std::string>, DnsErrorInfo>), resolve,
                (std::string_view host, RecordKind type, const Utils::CancellationToken &token), (const, override));
};

class MockIpSourcePort final : public IpSourcePort {
public:
    MOCK_METHOD((std::expected<std::vector<InetAddress>, domain::IpSourceError>), resolve,
                (const domain::SubdomainConfig &config, const Utils::CancellationToken &token), (const, override));
};

class MockDriverGateway final : public DriverGateway {
public:
    MOCK_METHOD((std::expected<void, domain::DriverError>), update,
                (std::string_view driver_name, const DriverUpdateCommand &command),
                (const, override));
};

class MockNetworkInterfaces final : public NetworkInterfaces {
public:
    MOCK_METHOD(std::vector<std::string>, names, (), (const, override));
    MOCK_METHOD((std::vector<InetAddress>), addresses, (const std::string &name), (const, override));
};

class MockDriverCatalogPort final : public DriverCatalogPort {
public:
    MOCK_METHOD(std::vector<std::string>, loaded_drivers, (), (const, override));
    MOCK_METHOD(DriverDescription, describe, (std::string_view name), (const, override));
};

#endif // YADDNSC_TEST_MOCKS_MOCK_PORTS_H
