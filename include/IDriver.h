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

using driver_config_t = std::map<std::string, std::string>;
using driver_param_t = std::multimap<std::string, std::string>;

enum class driver_http_method_t {
    GET, POST, PUT
};

struct driver_detail_t final {
    const std::string_view name;
    const std::string_view description;
    const std::string_view author;
    const std::string_view version;
};

struct driver_request_t {
    std::string url;
    std::variant<driver_param_t, std::string> body;
    std::string content_type;
    driver_http_method_t request_method;
    driver_param_t header;
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
    [[nodiscard]] virtual driver_request_t generate_request(const driver_config_t &) const = 0;

    [[nodiscard]] virtual bool check_response(std::string_view) const = 0;

    [[nodiscard]] virtual constexpr driver_detail_t get_detail() const = 0;

    [[nodiscard]] virtual constexpr std::string_view get_driver_version() const = 0;

    virtual void init_logger(int, std::string_view) = 0;

protected:
    std::vector<std::string> _required_param;
};

#endif //YADDNSC_IDRIVER_H
