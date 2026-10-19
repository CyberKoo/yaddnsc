#include "persistent_client.h"

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "infrastructure/http/error.h"
#include "infrastructure/http/redirect_exchange.h"
#include "infrastructure/http/wire_request.h"
#include "infrastructure/uri/uri.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"

namespace http {
namespace {

[[nodiscard]] Origin parse_origin(const std::string& base_url) {
    auto parsed = Uri::parse(base_url);
    const std::string_view scheme = parsed.has_value() ? parsed->get_schema() : std::string_view{};
    if (!parsed.has_value() || (scheme != "http" && scheme != "https") || parsed->get_host().empty()) {
        throw std::invalid_argument(fmt::format(R"(invalid base URL: "{}")", base_url));
    }
    return {.scheme = std::string(parsed->get_schema()),
            .host = std::string(parsed->get_host()),
            .port = static_cast<std::uint16_t>(parsed->get_port() > 0 ? parsed->get_port() : default_port(scheme))};
}

}  // namespace

PersistentClient::PersistentClient(const std::string& base_url, Options options)
    : PersistentClient(parse_origin(base_url), std::move(options)) {}

PersistentClient::PersistentClient(Origin origin, Options options)
    : scheme_(std::move(origin.scheme)), host_(std::move(origin.host)), port_(origin.port),
      options_(std::move(options)), session_(options_, scheme_, host_, port_) {}

PersistentClient::~PersistentClient() = default;

coro::Task<std::expected<Response, Error>> PersistentClient::exchange(std::string target, const Request& request) {
    // The client owns the request it is handed, so it validates it once here;
    // the redirect loop and the session treat validity as an invariant.
    if (auto valid = validate_request(request); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }
    std::string resolved_target = "/";
    if (!target.empty()) {
        auto parsed = Uri::parse(target);
        if (!parsed) {
            co_return std::unexpected(Error{ErrorCode::INVALID_URL, fmt::format(R"(invalid URL: "{}")", redacted_url(target))});
        }
        resolved_target = parsed->get_schema().empty() ? target : make_target(*parsed);
    }
    co_return co_await detail::exchange_redirects(make_absolute_url(scheme_, host_, port_, resolved_target), request,
                                                  options_, &session_);
}

void PersistentClient::close() noexcept {
    session_.close();
}

}  // namespace http
