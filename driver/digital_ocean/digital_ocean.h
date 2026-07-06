//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H
#define YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H

#include "driver/base.h"

class DigitalOceanDriver final : public BaseDriver {
public:
    ~DigitalOceanDriver() override = default;

    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const override;

    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    [[nodiscard]] DriverDetail get_detail() const override;
};

#endif //YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H
