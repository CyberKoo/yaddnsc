#include "client.h"

#include <utility>

#include "infrastructure/http/redirect_exchange.h"
#include "infrastructure/http/wire_request.h"

namespace http {

Client::Client(Options options) : options_(std::move(options)) {}

coro::Task<std::expected<Response, Error>> Client::exchange(std::string url, const Request& request) {
    // The client owns the request it is handed, so it validates it once here;
    // the redirect loop and the hop pipeline treat validity as an invariant.
    if (auto valid = validate_request(request); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }
    co_return co_await detail::exchange_redirects(std::move(url), request, options_);
}

}  // namespace http
