//
// MockHttpClient — GoogleMock-based mock for the HttpClient interface.
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_MOCK_HTTP_CLIENT_H
#define YADDNSC_TEST_MOCKS_MOCK_HTTP_CLIENT_H

#include <string_view>

#include <gmock/gmock.h>

#include "interface/http_client.h"

class MockHttpClient : public HttpClient {
public:
    MOCK_METHOD((std::expected<net::http::Response, net::http::Error>), exchange,
                (std::string_view url, const net::http::Request& req), (const, override));
};

#endif // YADDNSC_TEST_MOCKS_MOCK_HTTP_CLIENT_H
