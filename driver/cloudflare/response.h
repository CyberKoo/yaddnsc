//
// Created by Kotarou on 2026/6/17.
//
#ifndef YADDNSC_DRV_CLOUDFLARE_RESPONSE_H
#define YADDNSC_DRV_CLOUDFLARE_RESPONSE_H

#include <cstdint>
#include <string>
#include <optional>
#include <vector>

#include <glaze/glaze.hpp>

struct CloudflareSource {
    std::string pointer;
};

struct CloudflareErrorDetail {
    int64_t code = 0;
    std::string message;
    std::optional<std::string> documentation_url;
    std::optional<CloudflareSource> source;
};

struct CloudflareMessage {
    int64_t code = 0;
    std::string message;
    std::optional<std::string> documentation_url;
    std::optional<CloudflareSource> source;
};

struct CloudflareDnsRecord {
    std::string id;
    std::string name;
    std::string type;
    std::string content;
    int64_t ttl = 0;
    bool proxied = false;
    bool proxiable = false;
    std::optional<bool> private_routing;
    std::optional<std::string> comment;
    std::optional<std::vector<std::string>> tags;
};

struct CloudflareResponse {
    bool success = false;
    std::vector<CloudflareErrorDetail> errors;
    std::vector<CloudflareMessage> messages;
    std::optional<CloudflareDnsRecord> result;
};

template <>
struct glz::meta<CloudflareSource> {
    using T = CloudflareSource;
    static constexpr auto value = object(
        "pointer", &T::pointer
    );
};

template <>
struct glz::meta<CloudflareErrorDetail> {
    using T = CloudflareErrorDetail;
    static constexpr auto value = object(
        "code", &T::code,
        "message", &T::message,
        "documentation_url", &T::documentation_url,
        "source", &T::source
    );
};

template <>
struct glz::meta<CloudflareMessage> {
    using T = CloudflareMessage;
    static constexpr auto value = object(
        "code", &T::code,
        "message", &T::message,
        "documentation_url", &T::documentation_url,
        "source", &T::source
    );
};

template <>
struct glz::meta<CloudflareDnsRecord> {
    using T = CloudflareDnsRecord;
    static constexpr auto value = object(
        "id", &T::id,
        "name", &T::name,
        "type", &T::type,
        "content", &T::content,
        "ttl", &T::ttl,
        "proxied", &T::proxied,
        "proxiable", &T::proxiable,
        "private_routing", &T::private_routing,
        "comment", &T::comment,
        "tags", &T::tags
    );
};

template <>
struct glz::meta<CloudflareResponse> {
    using T = CloudflareResponse;
    static constexpr auto value = object(
        "success", &T::success,
        "errors", &T::errors,
        "messages", &T::messages,
        "result", &T::result
    );
};

#endif // YADDNSC_DRV_CLOUDFLARE_RESPONSE_H
