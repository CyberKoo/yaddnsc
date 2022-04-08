//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_IDRIVER_H
#define YADDNSC_IDRIVER_H

#include <map>
#include <string>
#include <string_view>

enum class request_method_t {
    GET, POST, PUT
};

struct driver_detail_t final {
    const std::string_view name;
    const std::string_view description;
    const std::string_view author;
    const std::string_view version;
};

struct request_t {
    std::string url;
    std::string body;
    std::string content_type;
    request_method_t request_method;
    std::multimap<std::string, std::string> header;
};

using driver_config_t = std::map<std::string, std::string>;

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
    virtual request_t generate_request(const driver_config_t &) = 0;

    virtual bool check_response(std::string_view) = 0;

    virtual driver_detail_t get_detail() = 0;

    virtual std::string_view get_driver_version() = 0;

    virtual void init_logger(int, std::string_view) = 0;

protected:
    std::vector<std::string> required_param;
};

#endif //YADDNSC_IDRIVER_H
