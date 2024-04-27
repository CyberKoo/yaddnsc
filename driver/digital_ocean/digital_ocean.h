//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DIGITAL_OCEAN_H
#define YADDNSC_DIGITAL_OCEAN_H

#include "../base_driver.h"

class DigitalOceanDriver final : public BaseDriver {
public:
    DigitalOceanDriver();

    ~DigitalOceanDriver() override = default;

    [[nodiscard]] driver_request generate_request(const driver_config_type &config) const override;

    [[nodiscard]] bool check_response(std::string_view response) const override;

    [[nodiscard]] driver_detail get_detail() const override;
};

extern "C" inline IDriver *create() {
    return new DigitalOceanDriver;
}

#endif //YADDNSC_DIGITAL_OCEAN_H
