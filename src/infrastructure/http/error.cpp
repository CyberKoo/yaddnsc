#include "error.h"

#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"

namespace http {

Error connection_error(const std::string_view stage) {
    return {ErrorCode::CONNECTION_LOST, fmt::format("{}: connection lost", stage)};
}

Error connect_error(const std::string_view host) {
    return {ErrorCode::CONNECT_FAILED, fmt::format(R"(Cannot connect to "{}")", host)};
}

std::string failure_reason(const std::string_view role, const Error& error) {
    switch (error.code) {
        case ErrorCode::RESOLVE_FAILED:
            return fmt::format("cannot resolve the {}", role);
        case ErrorCode::QUEUE_TIMEOUT:
            return fmt::format("the local session for the {} remained busy", role);
        case ErrorCode::CONNECT_FAILED:
            return fmt::format("cannot connect to the {}", role);
        case ErrorCode::TLS_HANDSHAKE_FAILED:
            return fmt::format("TLS handshake with the {} failed", role);
        case ErrorCode::CONNECTION_LOST:
            return fmt::format("the connection to the {} was lost", role);
        case ErrorCode::RESPONSE_PARSE_FAILED:
            return fmt::format("the {} returned an invalid response", role);
        case ErrorCode::HEADERS_TOO_LARGE:
        case ErrorCode::BODY_TOO_LARGE:
            return fmt::format("the {} returned an oversized response", role);
        case ErrorCode::REDIRECT_LIMIT_EXCEEDED:
            return fmt::format("the {} redirected too many times", role);
        case ErrorCode::INVALID_URL:
            return "the configured URL is invalid";
        case ErrorCode::INVALID_REQUEST:
        case ErrorCode::UNSUPPORTED_PROTOCOL:
            return "the request is invalid or unsupported";
    }
    return fmt::format("the request to the {} failed", role);
}

std::string redacted_url(const std::string_view url) {
    return std::string(url.substr(0, url.find('?')));
}

}  // namespace http
