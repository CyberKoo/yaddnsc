#include "redirect_exchange.h"

#include <cstdint>
#include <utility>

#include "infrastructure/http/error.h"
#include "infrastructure/http/redirect.h"
#include "infrastructure/http/session.h"
#include "infrastructure/http/wire_request.h"
#include "infrastructure/uri/uri.h"
#include "support/fmt.hpp"
#include "yaddnsc/util/format.hpp"

namespace http::detail {
namespace {

[[nodiscard]] std::uint16_t port_of(const Uri& uri) {
    return static_cast<std::uint16_t>(uri.get_port() > 0 ? uri.get_port() : default_port(uri.get_schema()));
}

coro::Task<std::expected<Response, Error>> exchange_hop(const Uri& uri, const Request& request, const Options& options,
                                                        Session* session) {
    const auto target = make_target(uri);
    if (session != nullptr) {
        co_return co_await session->exchange(target, request);
    }
    // A hop that cannot use the persistent origin still runs the same pipeline —
    // resolve, connect, replay, keep-alive accounting — on a session that lives
    // exactly as long as the hop and therefore shares no connection with it.
    Session hop{options, std::string(uri.get_schema()), std::string(uri.get_host()), port_of(uri)};
    co_return co_await hop.exchange(target, request);
}

}  // namespace

coro::Task<std::expected<Response, Error>> exchange_redirects(std::string url, Request request, const Options& options,
                                                              Session* initial_session) {
    // `request` is an invariant here: the entry point that owns it validated it
    // once, and a redirect only rewrites method, body and credential fields of
    // an already-valid request. The wire request each hop builds is checked by
    // the builder, which is where an untrusted redirect target is refused.
    for (int redirect_count = 0;; ++redirect_count) {
        const auto uri = Uri::parse(url);
        if (!uri || (uri->get_schema() != "http" && uri->get_schema() != "https") || uri->get_host().empty()) {
            // The query string can carry credentials; the redacted form keeps
            // them out of any log this message is streamed into.
            co_return std::unexpected(Error{ErrorCode::INVALID_URL, initial_session != nullptr
                                                                        ? "invalid request URL"
                                                                        : fmt::format(R"(invalid URL: "{}")", redacted_url(url))});
        }
        auto response = co_await exchange_hop(*uri, request, options, initial_session);
        if (!response) {
            co_return std::unexpected(std::move(response.error()));
        }
        auto eval = evaluate_redirect(response->status_, response->headers_, redirect_count, options, request, *uri);
        if (!eval.plan) {
            if (eval.limit_reached) {
                co_return std::unexpected(Error{ErrorCode::REDIRECT_LIMIT_EXCEEDED, "redirect limit exceeded"});
            }
            co_return std::move(*response);
        }
        if (eval.plan->cross_origin) {
            initial_session = nullptr;
        }
        request = std::move(eval.plan->next);
        url = std::move(eval.plan->url);
    }
}

}  // namespace http::detail
