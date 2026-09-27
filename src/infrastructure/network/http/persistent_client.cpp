//
// PersistentClient — the net::http HTTP client with connection reuse.
//
#include "infrastructure/network/http/persistent_client.h"

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <expected>
#include <yaddnsc/util/format.hpp>

#include "infrastructure/network/http/client.h"
#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/protocol/wire.h"
#include "infrastructure/network/http/redirect.h"
#include "infrastructure/network/http/stream_factory.h"
#include "infrastructure/network/http/wire_request.h"
#include "infrastructure/network/transport/options.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

namespace net::http {

namespace {

/// Strip the auto-generated headers from a wire request so it can be
/// replayed as a public Request (used for cross-origin redirect hops).
[[nodiscard]] Request to_request(const protocol::WireRequest& wire) {
    Request req;
    req.method = wire.method;
    req.body = wire.body;
    for (const auto& [name, value] : wire.headers) {
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

/// Parse and validate the client's base URL exactly once.
/// @throws std::invalid_argument when base_url is not a valid http(s) URL —
///         a precondition violation the caller cannot recover from here.
[[nodiscard]] Uri parse_validated_base_url(const std::string& base_url) {
    auto parsed = Uri::parse(base_url);
    const auto scheme = parsed ? parsed->get_schema() : std::string_view{};
    if (!parsed || (scheme != "http" && scheme != "https") || parsed->get_host().empty()) {
        throw std::invalid_argument(fmt::format(R"(invalid base URL: "{}")", base_url));
    }
    return std::move(*parsed);
}

}  // namespace

PersistentClient::PersistentClient(std::string base_url, Options opts)
    : PersistentClient(std::move(base_url), std::move(opts), std::make_shared<DefaultStreamFactory>()) {}

PersistentClient::PersistentClient(std::string base_url, Options opts, std::shared_ptr<StreamFactory> factory)
    : base_uri_(parse_validated_base_url(base_url)), scheme_(std::string(base_uri_.get_schema())),
      host_(std::string(base_uri_.get_host())),
      port_(static_cast<std::uint16_t>(base_uri_.get_port() > 0 ? base_uri_.get_port() : default_port(scheme_))),
      opts_(std::move(opts)), factory_(std::move(factory)),
      session_({factory_, opts_.transport, opts_.tls}, {scheme_, host_, port_}, opts_.limits) {}

PersistentClient::~PersistentClient() = default;

Uri PersistentClient::current_uri(const std::string_view target) const {
    // The URL is assembled from already-validated components (origin from the
    // validated base URL, target from the request), so parsing cannot fail.
    return Uri::parse(fmt::format("{}://{}:{}{}", scheme_,
                                  host_.find(':') != std::string::npos ? fmt::format("[{}]", host_) : host_, port_,
                                  target.empty() ? "/" : std::string(target)))
        .value();
}

std::expected<Response, Error> PersistentClient::exchange(const std::string_view url, const Request& req,
                                                          const Utils::CancellationToken& token) const {
    if (auto valid = validate_request(req); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    // Target semantics: a path ("/ip?x=1") is used verbatim; an absolute
    // http(s) URL contributes only its path+query — the origin always comes
    // from the base URL. (This is what makes the class usable through the
    // HttpClient port, where callers pass full URLs.)
    std::string target = "/";
    if (!url.empty()) {
        const auto parsed = Uri::parse(url);
        if (!parsed.has_value()) {
            return std::unexpected(Error{ErrorCode::INVALID_URL, fmt::format(R"(invalid URL: "{}")", url)});
        }
        target = parsed->get_schema().empty() ? std::string(url) : make_target(*parsed);
    }

    auto wire = build_wire_request(req, scheme_, host_, port_, opts_);
    wire.target = std::move(target);

    for (int redirect_count = 0;; ++redirect_count) {
        auto raw = session_.exchange(wire, token);
        if (!raw) {
            return std::unexpected(std::move(raw.error()));
        }

        const auto eval =
            evaluate_redirect(raw->status, raw->headers, redirect_count, opts_, wire, current_uri(wire.target));
        if (!eval.plan.has_value()) {
            if (eval.limit_reached) {
                return std::unexpected(Error{ErrorCode::REDIRECT_LIMIT_EXCEEDED, "redirect limit exceeded"});
            }
            return std::move(*raw);
        }

        auto& plan = *eval.plan;
        if (plan.cross_origin) {
            // The persistent connection cannot serve another origin —
            // follow the redirect with a one-shot transient exchange on the
            // same factory (fakes propagate).
            const auto absolute =
                fmt::format("{}://{}:{}{}", plan.scheme,
                            plan.host.find(':') != std::string::npos ? fmt::format("[{}]", plan.host) : plan.host,
                            plan.port, plan.next.target);
            Client transient(opts_, factory_);
            return transient.exchange(absolute, to_request(plan.next), token);
        }

        // Same origin: keep the connection and follow on it.
        wire = plan.next;
    }
}

}  // namespace net::http
