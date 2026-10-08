//
// http — transient client: one connection per hop, full redirect loop.
//

#include "client.h"

#include <cstdint>
#include <string>
#include <utility>

#include "infrastructure/net/http/protocol/exchange.h"
#include "infrastructure/net/http/redirect.h"
#include "infrastructure/net/http/transport.h"
#include "infrastructure/net/http/wire_request.h"
#include "infrastructure/network/uri.h"
#include "support/fmt.hpp"

namespace http {
namespace {

/// Reassemble an absolute URL from validated parts (bracketing an IPv6 host).
[[nodiscard]] std::string format_url(const std::string_view scheme, const std::string_view host,
                                     const std::uint16_t port, const std::string_view target) {
    const bool ipv6 = host.find(':') != std::string_view::npos;
    return fmt::format("{}://{}{}{}:{}{}", scheme, ipv6 ? "[" : "", host, ipv6 ? "]" : "", port, target);
}

}  // namespace

Client::Client(Options options) : options_(std::move(options)) {}

coro::Task<std::expected<Response, Error>> Client::exchange(std::string url, const Request& request) {
    if (auto valid = validate_request(request); !valid) {
        co_return std::unexpected(std::move(valid.error()));
    }

    auto wire_request = request;
    for (int redirect_count = 0;; ++redirect_count) {
        auto parsed = Uri::parse(url);
        if (!parsed.has_value()) {
            co_return std::unexpected(Error{ErrorCode::INVALID_URL, fmt::format(R"(invalid URL: "{}")", url)});
        }
        const std::string scheme{parsed->get_schema()};
        if ((scheme != "http" && scheme != "https") || parsed->get_host().empty()) {
            co_return std::unexpected(Error{ErrorCode::INVALID_URL, fmt::format(R"(invalid URL: "{}")", url)});
        }
        const std::string host{parsed->get_host()};
        const auto port =
            static_cast<std::uint16_t>(parsed->get_port() > 0 ? parsed->get_port() : default_port(scheme));

        auto addresses = co_await resolve_host(host, options_);
        if (!addresses) {
            co_return std::unexpected(std::move(addresses.error()));
        }
        auto stream = co_await connect_stream(scheme, *addresses, port, options_);
        if (!stream) {
            co_return std::unexpected(std::move(stream.error()));
        }

        auto wire = build_wire_request(wire_request, scheme, host, port, options_);
        wire.target = make_target(*parsed);

        auto raw = co_await protocol::exchange(**stream, wire, options_.limits);
        if (!raw) {
            co_return std::unexpected(std::move(raw.error()));
        }

        auto eval = evaluate_redirect(raw->status, raw->headers, redirect_count, options_, wire, *parsed);
        if (!eval.plan.has_value()) {
            if (eval.limit_reached) {
                co_return std::unexpected(Error{ErrorCode::REDIRECT_LIMIT_EXCEEDED, "redirect limit exceeded"});
            }
            co_return Response{raw->status, std::move(raw->body), std::move(raw->headers), std::move(raw->trailers)};
        }

        // Follow: a new origin gets a new connection, and the request is rebuilt
        // for it so the Host header and the body framing stay correct.
        auto& plan = *eval.plan;
        const std::string next_url = format_url(plan.scheme, plan.host, plan.port, plan.next.target);
        wire_request = to_public_request(plan.next);
        url = next_url;
    }
}

}  // namespace http
