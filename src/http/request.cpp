//
// Created by Kotarou on 2026/7/18.
//
#include "request.h"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <string>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "fmt.hpp"
#include "string_util.hpp"

namespace {

/// Check whether a header key is "Host" (case-insensitive).
[[nodiscard]] bool is_host_key(std::string_view key) noexcept {
    return StringUtil::iequals(key, "Host");
}

} // anonymous namespace

namespace Http {

std::vector<std::uint8_t> build_request(
    const HttpRequest &req,
    std::string_view path,
    std::string_view host_header,
    std::string_view user_agent) {

    // ── Body size ──
    const bool has_body = req.body.has_value() && !req.body->empty();
    const size_t body_size = has_body ? req.body->size() : 0;

    // ── Build headers ──
    std::string header_block;
    header_block.reserve(512);

    // Method name (magic_enum maps DEL→"DEL", but wire format needs "DELETE").
    auto method_name = magic_enum::enum_name(req.method);
    if (req.method == HttpMethod::DEL) {
        method_name = "DELETE";
    }

    // Request line.
    header_block += fmt::format("{} {} HTTP/1.1\r\n", method_name, path);

    // Host (only add if not already present in req.headers).
    bool has_host = false;
    for (const auto &kv : req.headers) {
        if (is_host_key(kv.first)) {
            has_host = true;
            break;
        }
    }
    
    if (!has_host) {
        header_block += fmt::format("Host: {}\r\n", host_header);
    }

    // User-Agent.
    header_block += fmt::format("User-Agent: {}\r\n", user_agent);

    // Content-Type (when body is present).
    if (has_body && !req.content_type.empty()) {
        header_block += fmt::format("Content-Type: {}\r\n", req.content_type);
    }

    // Content-Length (when body is present).
    if (has_body) {
        header_block += fmt::format("Content-Length: {}\r\n", body_size);
    }

    // Custom headers.
    for (const auto &kv : req.headers) {
        // Skip Host only if the default was already written above.
        if (!has_host && is_host_key(kv.first)) {
            continue;
        }
        header_block += fmt::format("{}: {}\r\n", kv.first, kv.second);
    }

    // End of headers.
    header_block += "\r\n";

    // ── Assemble wire bytes ──
    std::vector<std::uint8_t> wire;
    wire.reserve(header_block.size() + body_size);
    wire.insert(wire.end(),
                reinterpret_cast<const std::uint8_t *>(header_block.data()),
                reinterpret_cast<const std::uint8_t *>(header_block.data() + header_block.size()));
    if (has_body) {
        wire.insert(wire.end(),
                    reinterpret_cast<const std::uint8_t *>(req.body->data()),
                    reinterpret_cast<const std::uint8_t *>(req.body->data() + req.body->size()));
    }
    return wire;
}

} // namespace Http
