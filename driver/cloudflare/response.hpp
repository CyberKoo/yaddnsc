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

/// Cloudflare API error source location.
struct CloudflareSource {
    std::string pointer; ///< JSON pointer to the source of the error
};

/// Cloudflare API error detail.
struct CloudflareErrorDetail {
    int64_t code = 0;                              ///< Error code
    std::string message;                           ///< Error message
    std::optional<std::string> documentation_url;  ///< Link to documentation
    std::optional<CloudflareSource> source;        ///< Error source location
};

/// Cloudflare API informational message.
struct CloudflareMessage {
    int64_t code = 0;                              ///< Message code
    std::string message;                           ///< Message text
    std::optional<std::string> documentation_url;  ///< Link to documentation
    std::optional<CloudflareSource> source;        ///< Message source location
};

/// Cloudflare DNS record as returned by the API.
struct CloudflareDnsRecord {
    std::string id;                                    ///< Record ID
    std::string name;                                  ///< Domain name
    std::string type;                                  ///< Record type (A, AAAA, etc.)
    std::string content;                               ///< Record value
    int64_t ttl = 0;                                   ///< TTL in seconds
    bool proxied = false;                              ///< Whether proxied through Cloudflare
    bool proxiable = false;                            ///< Whether the record can be proxied
    std::optional<bool> private_routing;               ///< Whether Cloudflare Tunnel is used
    std::optional<std::string> comment;                ///< Record comment
    std::optional<std::vector<std::string>> tags;      ///< Record tags
};

/// Top-level Cloudflare API response.
struct CloudflareResponse {
    bool success = false;                          ///< Whether the API call succeeded
    std::vector<CloudflareErrorDetail> errors;     ///< Error details
    std::vector<CloudflareMessage> messages;       ///< Informational messages
    std::optional<CloudflareDnsRecord> result;     ///< DNS record data (present on success)
};

template<>
struct glz::meta<CloudflareSource> {
    using T = CloudflareSource;
    static constexpr auto value = object(
        "pointer", &T::pointer
    );
};

template<>
struct glz::meta<CloudflareErrorDetail> {
    using T = CloudflareErrorDetail;
    static constexpr auto value = object(
        "code", &T::code,
        "message", &T::message,
        "documentation_url", &T::documentation_url,
        "source", &T::source
    );
};

template<>
struct glz::meta<CloudflareMessage> {
    using T = CloudflareMessage;
    static constexpr auto value = object(
        "code", &T::code,
        "message", &T::message,
        "documentation_url", &T::documentation_url,
        "source", &T::source
    );
};

template<>
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

template<>
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
