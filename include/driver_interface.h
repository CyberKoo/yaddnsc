//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_INTERFACE_H
#define YADDNSC_DRIVER_INTERFACE_H

#include <span>
#include <string>
#include <flat_map>
#include <string_view>
#include <variant>

using driver_config_type = std::flat_map<std::string, std::string>;
using driver_param_type = std::multimap<std::string, std::string>;

enum class driver_http_method_type {
    GET, POST, PUT, PATCH, DELETE
};

struct driver_detail final {
    const std::string_view name;
    const std::string_view description;
    const std::string_view author;
    const std::string_view version;
};

struct driver_request {
    std::string url;
    std::variant<driver_param_type, std::string> body;
    std::string content_type;
    driver_http_method_type request_method;
    driver_param_type header;
};

class IDriver {
public:
    // Default construct
    IDriver() = default;

    // deconstruct
    virtual ~IDriver() = default;

    // delete copy and move constructors and assign operators
    // Copy construct
    IDriver(IDriver const &) = delete;

    // Move construct
    IDriver(IDriver &&) = delete;

    // Copy assign
    IDriver &operator=(IDriver const &) = delete;

    // Move assign
    IDriver &operator=(IDriver &&) = delete;

public:
    [[nodiscard]] virtual driver_request generate_request(const driver_config_type &) const = 0;

    [[nodiscard]] virtual bool check_response(std::string_view) const = 0;

    [[nodiscard]] virtual driver_detail get_detail() const = 0;

    [[nodiscard]] virtual std::string_view get_driver_version() const = 0;

    virtual void init_logger(int, const std::string &) {
    }

protected:
    [[nodiscard]] virtual std::span<const std::string_view> get_required_params() const = 0;
};

#endif //YADDNSC_DRIVER_INTERFACE_H
