//
// MockResolver — GoogleMock-based mock for the ResolverBase interface.
//
// Provides configurable expectations for query() and get_type() so that
// the dispatcher and other DNS components can be tested without a real
// DNS resolver.
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_MOCK_RESOLVER_H
#define YADDNSC_TEST_MOCKS_MOCK_RESOLVER_H

#include <string_view>

#include <gmock/gmock.h>

#include "dns/resolver/base.h"

class MockResolver : public ResolverBase {
public:
    MOCK_METHOD(std::expected<std::vector<std::uint8_t>, DnsLookupException>,
                query,
                (const std::string& host, RecordKind type, int cancel_fd),
                (const, noexcept, override));

    MOCK_METHOD(std::string_view, get_type, (), (const, noexcept, override));
};

#endif // YADDNSC_TEST_MOCKS_MOCK_RESOLVER_H
