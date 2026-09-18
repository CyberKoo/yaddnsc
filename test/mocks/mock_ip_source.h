//
// MockIpSource — GoogleMock-based mock for the IpSourceBase interface.
//
// Allows tests to control IP source resolution outcomes (address found,
// multiple addresses, empty result, structured failure) without real network
// interfaces or HTTP/mDNS servers.
//
// Usage:
//   MockIpSource mock;
//   EXPECT_CALL(mock, resolve(_))
//       .WillOnce(Return(IpSourceBase::Result{std::vector{InetAddress::parse("192.168.1.1").value()}}));
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_MOCK_IP_SOURCE_H
#define YADDNSC_TEST_MOCKS_MOCK_IP_SOURCE_H

#include <vector>

#include <gmock/gmock.h>

#include "infrastructure/ip_source/base.h"
#include "domain/network/inet_address.h"

class MockIpSource : public IpSourceBase {
public:
    MOCK_METHOD(IpSourceBase::Result, resolve, (const Utils::CancellationToken& token), (const, override));
};

#endif  // YADDNSC_TEST_MOCKS_MOCK_IP_SOURCE_H
