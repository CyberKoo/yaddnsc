//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DIGITAL_OCEAN_H
#define YADDNSC_DIGITAL_OCEAN_H

#include "../driver.h"

class DigitalOceanDriver final : public Driver {
public:
    DigitalOceanDriver();

    ~DigitalOceanDriver() override = default;

    driver_request_t generate_request(const driver_config_t &config) override;

    bool check_response(std::string_view response) override;

    driver_detail_t get_detail() override;
};

extern "C" [[maybe_unused]] IDriver *create() {
    return new DigitalOceanDriver;
}

#endif //YADDNSC_DIGITAL_OCEAN_H
