//
// Created by Kotarou on 2026/7/18.
//
#include "request.h"

#include <cstdint>
#include <string>
#include <vector>

#include <magic_enum/magic_enum.hpp>
#include <spdlog/spdlog.h>

#include "fmt.hpp"
#include "string_util.hpp"

namespace {

/// Check whether a header key is "Host" (case-insensitive).
[[nodiscard]] bool is_host_key(std::string_view key) noexcept {
    return StringUtil::iequals(key, "Host");
}

/// Strip CR and LF characters that would enable HTTP header injection.
/// Logs a warning if any were found.
[[nodiscard]] std::string sanitize_crlf(std::string_view s) {
    if (!StringUtil::contains(s, "\r") && !StringUtil::contains(s, "\n")) {
        return std::string(s);
    }
    // CR/LF found — strip them out using the project's replace utilities.
    auto result = StringUtil::replace_all_copy(s, "\r", "");
    StringUtil::replace_all(result, "\n", "");
    SPDLOG_WARN("CR/LF stripped from HTTP header value: \"{}\"", s);
    return result;
}

}  // anonymous namespace

namespace Http {

std::vector<std::uint8_t> build_request(const HttpRequest& req,
                                        std::string_view path,
                                        std::string_view host_header,
                                        std::string_view user_agent) {
    // ── Sanitize inputs against CRLF injection ──
    const auto safe_path = sanitize_crlf(path);
    const auto safe_host = sanitize_crlf(host_header);
    const auto safe_ua = sanitize_crlf(user_agent);
    const auto safe_ct = sanitize_crlf(req.content_type);

    // ── Body size ──
    const auto has_body = req.body.has_value() && !req.body->empty();
    const auto body_size = has_body ? req.body->size() : 0;

    // ── Build headers ──
    std::string header_block;
    header_block.reserve(512);

    // Method name (magic_enum maps DEL→"DEL", but wire format needs "DELETE").
    auto method_name = magic_enum::enum_name(req.method);
    if (req.method == HttpMethod::DEL) {
        method_name = "DELETE";
    }

    // Request line.
    header_block += fmt::format("{} {} HTTP/1.1\r\n", method_name, safe_path);

    // Host (only add if not already present in req.headers).
    auto has_host = false;
    for (const auto& kv : req.headers) {
        // Sanitize header name for host detection in case of CRLF injection.
        if (is_host_key(sanitize_crlf(kv.first))) {
            has_host = true;
            break;
        }
    }

    if (!has_host) {
        header_block += fmt::format("Host: {}\r\n", safe_host);
    }

    // User-Agent.
    header_block += fmt::format("User-Agent: {}\r\n", safe_ua);

    // Content-Type (when body is present).
    if (has_body && !safe_ct.empty()) {
        header_block += fmt::format("Content-Type: {}\r\n", safe_ct);
    }

    // Content-Length (when body is present).
    if (has_body) {
        header_block += fmt::format("Content-Length: {}\r\n", body_size);
    }

    // Custom headers.
    for (const auto& kv : req.headers) {
        const auto safe_key = sanitize_crlf(kv.first);
        // Skip Host only if the default was already written above.
        if (!has_host && is_host_key(safe_key)) {
            continue;
        }
        header_block += fmt::format("{}: {}\r\n", safe_key, sanitize_crlf(kv.second));
    }

    // End of headers.
    header_block += "\r\n";

    // ── Assemble wire bytes ──
    std::vector<std::uint8_t> wire;
    wire.reserve(header_block.size() + body_size);
    wire.insert(wire.end(), reinterpret_cast<const std::uint8_t*>(header_block.data()),
                reinterpret_cast<const std::uint8_t*>(header_block.data() + header_block.size()));
    if (has_body) {
        // Body is binary — do not sanitize CR/LF (DNS wire format uses 0x0A/0x0D legitimately).
        wire.insert(wire.end(), reinterpret_cast<const std::uint8_t*>(req.body->data()),
                    reinterpret_cast<const std::uint8_t*>(req.body->data() + req.body->size()));
    }
    return wire;
}

}  // namespace Http
