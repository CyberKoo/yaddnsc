//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H
#define YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H

#include <driver/base_driver.h>

#include "config.hpp"

class CloudflareDriver final : public BaseDriver {
public:
    ~CloudflareDriver() override = default;

    [[nodiscard]] driver_request generate_request(const driver_config_type &, const UpdateContext &) const override;

    [[nodiscard]] bool check_response(std::string_view) const override;

    [[nodiscard]] DriverDetail get_detail() const override;

private:
    static std::string generate_body(const CloudflareParams &, const UpdateContext &);
};

#endif //YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H
