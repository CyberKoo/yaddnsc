//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_SIMPLE_H
#define YADDNSC_SIMPLE_H

#include "../driver.h"

class SimpleDriver final : public Driver {
public:
    SimpleDriver();

    ~SimpleDriver() override = default;

    [[nodiscard]] driver_request_t generate_request(const driver_config_t &config) const override;

    [[nodiscard]] constexpr driver_detail_t get_detail() const override;

    [[nodiscard]] bool check_response(std::string_view) const override;

private:
    static std::map<std::string, std::string> get_format_params(const driver_config_t &config);
};

extern "C" [[maybe_unused]] IDriver *create() {
    return new SimpleDriver;
}

#endif //YADDNSC_SIMPLE_H
