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

    [[nodiscard]] driver_request generate_request(const driver_config_type &config) const override;

    [[nodiscard]] driver_detail get_detail() const override;

    [[nodiscard]] bool check_response(std::string_view) const override;

private:
    static std::map<std::string, std::string> get_format_params(const driver_config_type &config);
};

extern "C" [[maybe_unused]] IDriver *create() {
    return new SimpleDriver;
}

#endif //YADDNSC_SIMPLE_H
