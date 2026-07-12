//
// Created by Kotarou on 2026/6/18.
//

#ifndef YADDNSC_HTTP_FMT_H
#define YADDNSC_HTTP_FMT_H

#include "fmt.hpp"
#include "http_type.h"

/// fmt / std::format formatter for HttpRequest
/// (also covers DriverRequest, which is a type alias for HttpRequest).

#ifdef YADDNSC_USE_STD_FORMAT
template<>
struct std::formatter<HttpRequest> {
#else
    template<>
    struct fmt::formatter<HttpRequest> {

#endif

    /// Convert an HttpMethod enum value to its string representation.
    static std::string_view to_string(const HttpMethod type) {
        switch (type) {
            case HttpMethod::GET:
                return "GET";
            case HttpMethod::POST:
                return "POST";
            case HttpMethod::PUT:
                return "PUT";
            case HttpMethod::PATCH:
                return "PATCH";
            case HttpMethod::DEL:
                return "DELETE";
            case HttpMethod::HEAD:
                return "HEAD";
            case HttpMethod::OPTIONS:
                return "OPTIONS";
        }

        std::unreachable();
    }

    /// Format a key-value map range into a human-readable string.
    /// @param first  Iterator to the first key-value pair.
    /// @param last   Past-the-end iterator.
    /// @return       String like "key1=val1; key2=val2".
    template<typename Iter>
    [[nodiscard]] static std::string format_map(Iter first, Iter last) {
        std::string buf;

        for (auto it = first; it != last; ++it) {
            buf.append(it->first);
            buf.append("=");
            buf.append(it->second);
            buf.append("; ");
        }

        if (buf.size() >= 2) {
            buf.erase(buf.size() - 2);
        }

        return buf;
    }

    /// Parse the format specification.
    /// HttpRequest accepts no custom format options, so this always
    /// returns the end-of-spec iterator.
    /// @return  Iterator past the format spec (always ctx.begin()).
    static constexpr auto parse(format_parse_context &ctx) -> decltype(ctx.begin()) {
        return ctx.begin();
    }

    /// Format an HttpRequest into the output context.
    /// Renders as: HttpRequest(body="...", content_type="...", method="...", header="...")
    /// @param request  The HTTP request to format.
    /// @param ctx      The format output context.
    /// @return         Iterator past the last written character.
    template<typename FormatContext>
    auto format(const HttpRequest &request, FormatContext &ctx) const -> decltype(ctx.out()) {
        const auto &body = request.body.value_or("");

        return fmt::format_to(
            ctx.out(),
            R"(HttpRequest(body="{}", content_type="{}", method="{}", header="{}"))",
            body, request.content_type, to_string(request.method),
            format_map(request.headers.begin(), request.headers.end())
        );
    }
};

#endif //YADDNSC_HTTP_FMT_H
