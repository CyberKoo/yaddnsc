//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_URI_H
#define YADDNSC_URI_H

#include <string>
#include <optional>
#include <string_view>

class Uri {
public:
    static Uri parse(std::string_view uri);

    [[nodiscard]] std::string_view get_query_string() const;

    [[nodiscard]] std::string_view get_path() const;

    [[nodiscard]] std::string_view get_schema() const;

    [[nodiscard]] std::string_view get_host() const;

    [[nodiscard]] int get_port() const;

    [[nodiscard]] std::string_view get_body() const;

    [[nodiscard]] std::string_view get_raw_uri() const;

private:
    Uri() = default;

    static int default_port_for(std::string_view scheme) noexcept;

private:
    std::string query_string_;
    std::string path_;
    std::string schema_;
    std::string host_;
    std::string raw_uri_;
    std::string body_;
    std::optional<int> port_;     // nullopt = not explicitly set
};

#endif //YADDNSC_URI_H
