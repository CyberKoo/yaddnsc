//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H
#define YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H

#include "drivers/base_driver.h"

class DigitalOceanDriver final : public BaseDriver {
public:
    ~DigitalOceanDriver() override = default;

    [[nodiscard]] driver_request generate_request(
        const driver_config_type &config, const UpdateContext &ctx) const override;

    [[nodiscard]] bool check_response(const HttpResponseData &response) const override;

    [[nodiscard]] DriverDetail get_detail() const override;
};

#endif //YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H
