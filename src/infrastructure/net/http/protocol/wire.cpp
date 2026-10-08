//
// http — wire-level request serialization.
//

#include "wire.h"

#include <cstddef>

namespace http::protocol {

std::string_view method_name(const Method method) noexcept {
    switch (method) {
        case Method::GET:
            return "GET";
        case Method::POST:
            return "POST";
        case Method::PUT:
            return "PUT";
        case Method::DEL:
            return "DELETE";
        case Method::PATCH:
            return "PATCH";
        case Method::HEAD:
            return "HEAD";
        case Method::OPTIONS:
            return "OPTIONS";
    }
    return "GET";
}

std::string serialize(const WireRequest& request) {
    const std::size_t body_size = request.body.has_value() ? request.body->size() : 0;

    std::string out;
    out.reserve(64 + request.target.size() + body_size);

    out += method_name(request.method);
    out += ' ';
    out += request.target.empty() ? "/" : request.target;
    out += request.version == HttpVersion::V1_0 ? " HTTP/1.0\r\n" : " HTTP/1.1\r\n";

    for (const auto& [name, value] : request.headers) {
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    out += "\r\n";
    if (body_size > 0) {
        out += *request.body;
    }
    return out;
}

}  // namespace http::protocol
