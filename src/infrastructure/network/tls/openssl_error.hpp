//
// net — internal OpenSSL diagnostics shared by the TLS units.
//
// The OpenSSL error queue is thread-local: drain and format it on the thread
// that made the failing call.
//

#ifndef YADDNSC_INFRASTRUCTURE_NETWORK_TLS_OPENSSL_ERROR_HPP
#define YADDNSC_INFRASTRUCTURE_NETWORK_TLS_OPENSSL_ERROR_HPP

#include <array>
#include <string>

#include <openssl/err.h>

namespace net::detail {

/// The current OpenSSL error stack as one line, for diagnostics only.
[[nodiscard]] inline std::string ssl_errors() {
    std::string text;
    unsigned long error = 0;
    while ((error = ERR_get_error()) != 0) {
        std::array<char, 256> buffer{};
        ERR_error_string_n(error, buffer.data(), buffer.size());
        if (!text.empty()) {
            text.append("; ");
        }
        text.append(buffer.data());
    }
    return text;
}

}  // namespace net::detail

#endif  // YADDNSC_INFRASTRUCTURE_NETWORK_TLS_OPENSSL_ERROR_HPP
