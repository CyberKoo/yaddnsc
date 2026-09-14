//
// PersistentClient — the net::http HTTP client with connection reuse.
//
#include "http_client/persistent_client.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "http_client/client.h"
#include "http_client/redirect.h"
#include "http_client/wire_request.h"

#include "fmt.hpp"

namespace net::http {

namespace {

/// Strip the auto-generated headers from a wire request so it can be
/// replayed as a public Request (used for cross-origin redirect hops).
[[nodiscard]] Request to_request(const protocol::WireRequest& wire) {
    Request req;
    req.method = wire.method;
    req.body = wire.body;
    for (const auto& [name, value]: wire.headers) {
        if (name == "Host" || name == "User-Agent" || name == "Content-Length") {
            continue;
        }
        if (name == "Content-Type") {
            req.content_type = value;
            continue;
        }
        req.headers.emplace(name, value);
    }
    return req;
}

} // namespace

PersistentClient::PersistentClient(std::string base_url, Options opts)
    : PersistentClient(std::move(base_url), std::move(opts), std::make_shared<DefaultStreamFactory>()) {
}

PersistentClient::PersistentClient(std::string base_url, Options opts, Utils::CancellationToken token)
    : PersistentClient(std::move(base_url), std::move(opts),
                       std::static_pointer_cast<StreamFactory>(
                           std::make_shared<DefaultStreamFactory>(std::move(token)))) {
}

PersistentClient::PersistentClient(std::string base_url, Options opts, std::shared_ptr<StreamFactory> factory)
    : scheme_(std::string(Uri::parse(base_url).get_schema())),
      host_(std::string(Uri::parse(base_url).get_host())),
      port_(static_cast<std::uint16_t>(Uri::parse(base_url).get_port() > 0
                                            ? Uri::parse(base_url).get_port()
                                            : default_port(scheme_))),
      base_uri_(Uri::parse(base_url)), opts_(std::move(opts)), factory_(std::move(factory)),
      session_(factory_, opts_.transport, opts_.tls, scheme_, host_, port_, opts_.limits) {
    if ((scheme_ != "http" && scheme_ != "https") || host_.empty()) {
        throw std::invalid_argument(fmt::format(R"(invalid base URL: "{}")", base_url));
    }
}

PersistentClient::~PersistentClient() = default;

Uri PersistentClient::current_uri(const std::string_view target) const {
    return Uri::parse(fmt::format("{}://{}:{}{}", scheme_,
                                  host_.find(':') != std::string::npos ? fmt::format("[{}]", host_) : host_,
                                  port_, target.empty() ? "/" : std::string(target)));
}

std::expected<Response, Error> PersistentClient::exchange(const std::string_view url, const Request& req) const {
    // Target semantics: a path ("/ip?x=1") is used verbatim; an absolute
    // http(s) URL contributes only its path+query — the origin always comes
    // from the base URL. (This is what makes the class usable through the
    // HttpClient port, where callers pass full URLs.)
    std::string target = "/";
    if (!url.empty()) {
        const auto parsed = Uri::parse(url);
        target = parsed.get_schema().empty() ? std::string(url) : make_target(parsed);
    }

    auto wire = build_wire_request(req, scheme_, host_, port_, opts_);
    wire.target = std::move(target);

    for (int redirect_count = 0;; ++redirect_count) {
        auto raw = session_.exchange(wire);
        if (!raw) {
            return std::unexpected(std::move(raw.error()));
        }

        const auto eval = evaluate_redirect(raw->status, raw->headers, redirect_count, opts_, wire,
                                                      current_uri(wire.target));
        if (!eval.plan.has_value()) {
            if (eval.limit_reached) {
                return std::unexpected(Error{ErrorCode::REDIRECT_LIMIT_EXCEEDED, "redirect limit exceeded"});
            }
            return Response{
                .status = raw->status,
                .body = std::move(raw->body),
                .headers = std::move(raw->headers),
            };
        }

        auto& plan = *eval.plan;
        if (plan.cross_origin) {
            // The persistent connection cannot serve another origin —
            // follow the redirect with a one-shot transient exchange on the
            // same factory (token/fakes propagate).
            const auto absolute = fmt::format("{}://{}:{}{}", plan.scheme,
                                              plan.host.find(':') != std::string::npos
                                                  ? fmt::format("[{}]", plan.host)
                                                  : plan.host,
                                              plan.port, plan.next.target);
            Client transient(opts_, factory_);
            return transient.exchange(absolute, to_request(plan.next));
        }

        // Same origin: keep the connection and follow on it.
        wire = std::move(plan.next);
    }
}

} // namespace net::http
