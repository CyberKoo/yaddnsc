//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_HTTP_TYPE_FORMATTER_H
#define YADDNSC_HTTP_TYPE_FORMATTER_H

#include "http_types.h"
#include "fmt.hpp"

// ---------------------------------------------------------------------------
// fmt / std::format formatter for http_request
// (also covers driver_request, which is a type alias for http_request)
// ---------------------------------------------------------------------------

#ifdef YADDNSC_USE_STD_FORMAT
template<>
struct std::formatter<http_request> {
#else
template<>
struct fmt::formatter<http_request> {
#endif

    static std::string_view to_string(const http_method_type type) {
        switch (type) {
            case http_method_type::GET:
                return "GET";
            case http_method_type::POST:
                return "POST";
            case http_method_type::PUT:
                return "PUT";
            case http_method_type::PATCH:
                return "PATCH";
            case http_method_type::DEL:
                return "DELETE";
            default:
                return "UNKNOWN";
        }
    }

    template<typename Iter>
#ifdef YADDNSC_USE_STD_FORMAT
    [[nodiscard]] std::string format_map(Iter first, Iter last) const {
#else
    [[nodiscard]] std::string format_map(Iter first, Iter last) const {
#endif
        std::string buf;

        for (auto begin = first, it = begin, end = last; it != end; ++it) {
            buf.append(it->first);
            buf.append("=");
            buf.append(it->second);
            buf.append("; ");
        }

        if (!buf.empty() && buf.size() >= 2) {
            buf.erase(buf.end() - 2);
        }

        return buf;
    }

    static constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
        return ctx.begin();
    }

    template<typename FormatContext>
#ifdef YADDNSC_USE_STD_FORMAT
    auto format(const http_request &request, FormatContext &ctx) const -> decltype(ctx.out()) {
#else
    auto format(const http_request &request, FormatContext &ctx) const -> decltype(ctx.out()) {
#endif
        std::string body_type;
        std::string body;

        std::visit([&]<typename T>(const T &body_) {
                       if constexpr (std::is_same_v<T, http_param_type>) {
                           body_type = "map";
                           body = format_map(body_.begin(), body_.end());
                       } else if constexpr (std::is_same_v<T, std::string>) {
                           body_type = "string";
                           body = body_;
                       }
                   }, request.body
        );

#ifdef YADDNSC_USE_STD_FORMAT
        return std::format_to(ctx.out(),
                              R"(http_request(url="{}", body_type="{}", body="{}", content_type="{}", request_method="{}", header="{}"))",
                              request.url, body_type, body, request.content_type, to_string(request.request_method),
                              format_map(request.header.begin(), request.header.end()));
#else
        return fmt::format_to(ctx.out(),
                              R"(http_request(url="{}", body_type="{}", body="{}", content_type="{}", request_method="{}", header="{}"))",
                              request.url, body_type, body, request.content_type, to_string(request.request_method),
                              format_map(request.header.begin(), request.header.end()));
#endif
    }
};

#endif //YADDNSC_HTTP_TYPE_FORMATTER_H
