//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_IDRIVER_H
#define YADDNSC_IDRIVER_H

#include <map>
#include <string>
#include <vector>
#include <variant>
#include <string_view>

using driver_config_type = std::map<std::string, std::string>;
using driver_param_type = std::multimap<std::string, std::string>;

enum class driver_http_method_type {
    GET, POST, PUT
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

    // Default construct
    IDriver() = default;

public:
    [[nodiscard]] virtual driver_request generate_request(const driver_config_type &) const = 0;

    [[nodiscard]] virtual bool check_response(std::string_view) const = 0;

    [[nodiscard]] virtual driver_detail get_detail() const = 0;

    [[nodiscard]] virtual std::string_view get_driver_version() const = 0;

    virtual void init_logger(int, std::string_view) = 0;

protected:
    std::vector<std::string> required_param_;
};

#endif //YADDNSC_IDRIVER_H
