//
// http — persistent client: fixed origin, reused connection.
//

#include "persistent_client.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "infrastructure/net/http/client.h"
#include "infrastructure/net/http/redirect.h"
#include "infrastructure/net/http/wire_request.h"
#include "infrastructure/net/http/uri.h"
#include "support/fmt.hpp"

namespace http {
namespace {

/// An absolute URL for this origin (bracketing an IPv6 host).
[[nodiscard]] std::string origin_url(const std::string_view scheme, const std::string_view host,
                                     const std::uint16_t port, const std::string_view target) {
    const bool ipv6 = host.find(':') != std::string_view::npos;
    return fmt::format("{}://{}{}{}:{}{}", scheme, ipv6 ? "[" : "", host, ipv6 ? "]" : "", port, target);
}

/// Parse and validate the base URL once, at construction.
[[nodiscard]] Origin parse_origin(const std::string& base_url) {
    auto parsed = Uri::parse(base_url);
    const std::string_view scheme = parsed.has_value() ? parsed->get_schema() : std::string_view{};
    if (!parsed.has_value() || (scheme != "http" && scheme != "https") || parsed->get_host().empty()) {
        throw std::invalid_argument(fmt::format(R"(invalid base URL: "{}")", base_url));
    }
    Origin origin;
    origin.scheme = std::string(parsed->get_schema());
    origin.host = std::string(parsed->get_host());
    origin.port = static_cast<std::uint16_t>(parsed->get_port() > 0 ? parsed->get_port() : default_port(origin.scheme));
    return origin;
}

}  // namespace

PersistentClient::PersistentClient(std::string base_url, Options options)
    : PersistentClient(parse_origin(base_url), std::move(options)) {}

PersistentClient::PersistentClient(Origin origin, Options options)
    : scheme_(std::move(origin.scheme)), host_(std::move(origin.host)), port_(origin.port),
      options_(std::move(options)), session_(options_, scheme_, host_, port_) {}

PersistentClient::~PersistentClient() = default;

coro::Task<std::expected<Response, Error>> PersistentClient::exchange(std::string target, const Request& request) {
    if (auto valid = validate_request(request); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }

    // A bare path is used verbatim; an absolute URL contributes only its
    // path+query, because this client's origin is fixed at construction.
    std::string resolved_target = "/";
    if (!target.empty()) {
        auto parsed = Uri::parse(target);
        if (!parsed.has_value()) {
            co_return std::unexpected(Error{ErrorCode::INVALID_URL, fmt::format(R"(invalid URL: "{}")", target)});
        }
        resolved_target = parsed->get_schema().empty() ? target : make_target(*parsed);
    }

    Request current = request;
    std::string current_target = std::move(resolved_target);

    for (int redirect_count = 0;; ++redirect_count) {
        auto uri = Uri::parse(origin_url(scheme_, host_, port_, current_target));
        if (!uri.has_value()) {
            co_return std::unexpected(Error{ErrorCode::INVALID_URL, "invalid request URL"});
        }

        auto raw = co_await session_.exchange(current_target, current);
        if (!raw) {
            co_return std::unexpected(std::move(raw.error()));
        }

        auto wire = build_wire_request(current, scheme_, host_, port_, options_);
        wire.target = current_target;
        const auto eval = evaluate_redirect(raw->status_, raw->headers_, redirect_count, options_, wire, *uri);
        if (!eval.plan.has_value()) {
            if (eval.limit_reached) {
                co_return std::unexpected(Error{ErrorCode::REDIRECT_LIMIT_EXCEEDED, "redirect limit exceeded"});
            }
            co_return std::move(*raw);
        }

        const auto& plan = *eval.plan;
        if (plan.cross_origin) {
            // Another origin cannot be served by this session's connection: the
            // hop is followed with a transient exchange on the same factory.
            Client transient{options_};
            co_return co_await transient.exchange(origin_url(plan.scheme, plan.host, plan.port, plan.next.target),
                                                  to_public_request(plan.next));
        }

        // Same origin: keep the connection and continue on it.
        current = to_public_request(plan.next);
        current_target = plan.next.target;
    }
}

void PersistentClient::close() noexcept {
    session_.close();
}

}  // namespace http
