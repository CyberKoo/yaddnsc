//
// MockHttpClient — GoogleMock-based mock for the HttpClient interface.
//
// Provides configurable expectations for exchange() and delegates to the
// real get_body() and params_to_query_string() implementations.
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_MOCK_HTTP_CLIENT_H
#define YADDNSC_TEST_MOCKS_MOCK_HTTP_CLIENT_H

#include <string_view>

#include <gmock/gmock.h>

#include "interface/http_client.h"

class MockHttpClient : public HttpClient {
public:
    MOCK_METHOD(HttpResult, exchange, (std::string_view url, const HttpRequest& req), (const, override));

    // get_body() and params_to_query_string() are NOT virtual — they are
    // implemented in the HttpClient base class using exchange().  Tests
    // that need to verify get_body() calls should instead set expectations
    // on exchange() and let the base implementation delegate.
};

#endif // YADDNSC_TEST_MOCKS_MOCK_HTTP_CLIENT_H
