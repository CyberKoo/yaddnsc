//
// Client — the net::http HTTP client.
//
#include "infrastructure/network/http/client.h"

#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include <expected>
#include <yaddnsc/util/format.hpp>

#include "infrastructure/network/http/error.h"
#include "infrastructure/network/http/protocol/exchange.h"
#include "infrastructure/network/http/protocol/wire.h"
#include "infrastructure/network/http/redirect.h"
#include "infrastructure/network/http/stream_factory.h"
#include "infrastructure/network/http/wire_request.h"
#include "infrastructure/network/transport/stream.h"
#include "infrastructure/network/uri.h"
#include "support/fmt.hpp"
#include "support/util/cancellation_token.hpp"

namespace net::http {

Client::Client(Options opts) : Client(std::move(opts), std::make_shared<DefaultStreamFactory>()) {}

Client::Client(Options opts, std::shared_ptr<StreamFactory> factory)
    : opts_(std::move(opts)), factory_(std::move(factory)) {
    if (!factory_) {
        factory_ = std::make_shared<DefaultStreamFactory>();
    }
}

std::expected<Response, Error> Client::exchange(const std::string_view url,
                                                const Request& req,
                                                const Utils::CancellationToken& token) const {
    if (auto valid = validate_request(req); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    std::optional<Uri> parsed_uri;
    try {
        parsed_uri.emplace(Uri::parse(url));
    } catch (const std::exception&) {
        return std::unexpected(Error{ErrorCode::INVALID_URL, fmt::format(R"(invalid URL: "{}")", url)});
    }
    auto scheme = std::string(parsed_uri->get_schema());
    if ((scheme != "http" && scheme != "https") || parsed_uri->get_host().empty()) {
        return std::unexpected(Error{ErrorCode::INVALID_URL, fmt::format(R"(invalid URL: "{}")", url)});
    }
    auto host = std::string(parsed_uri->get_host());
    auto port = static_cast<std::uint16_t>(parsed_uri->get_port() > 0 ? parsed_uri->get_port() : default_port(scheme));

    auto wire = build_wire_request(req, scheme, host, port, opts_);
    wire.target = make_target(*parsed_uri);

    for (int redirect_count = 0;; ++redirect_count) {
        // The scheme/transport pairing is decided HERE — https -> TLS,
        // http -> TCP — not by the injected factory.
        std::unique_ptr<Transport::Stream> stream = scheme == "https"
                                                        ? factory_->create_tls(host, port, opts_.transport, opts_.tls)
                                                        : factory_->create_tcp(host, port, opts_.transport);
        if (auto connected = stream->ensure_connected(token); !connected) {
            return std::unexpected(map_connect_error(connected.error()));
        }

        auto raw = protocol::exchange(*stream, wire, opts_.limits, token);
        if (!raw) {
            return std::unexpected(std::move(raw.error()));
        }

        auto eval = evaluate_redirect(raw->status, raw->headers, redirect_count, opts_, wire, *parsed_uri);
        if (!eval.plan.has_value()) {
            if (eval.limit_reached) {
                return std::unexpected(Error{ErrorCode::REDIRECT_LIMIT_EXCEEDED, "redirect limit exceeded"});
            }
            return Response{raw->status, std::move(raw->body), std::move(raw->headers), std::move(raw->trailers)};
        }

        // Follow the redirect: possibly a new origin, always a new target.
        auto& plan = *eval.plan;
        wire = std::move(plan.next);
        scheme = std::move(plan.scheme);
        host = std::move(plan.host);
        port = plan.port;
        try {
            parsed_uri.emplace(Uri::parse(fmt::format(
                "{}://{}:{}{}", scheme, host.find(':') != std::string::npos ? fmt::format("[{}]", host) : host, port,
                wire.target)));
        } catch (const std::exception&) {
            return std::unexpected(Error{ErrorCode::INVALID_URL, "invalid redirect URL"});
        }
    }
}

}  // namespace net::http
