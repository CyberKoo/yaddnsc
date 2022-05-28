//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_CLOUDFLARE_H
#define YADDNSC_CLOUDFLARE_H

#include "../base_driver.h"

class CloudflareDriver final : public BaseDriver {
public:
    CloudflareDriver();

    ~CloudflareDriver() override = default;

    [[nodiscard]] driver_request generate_request(const driver_config_type &) const override;

    [[nodiscard]] bool check_response(std::string_view) const override;

    [[nodiscard]] driver_detail get_detail() const override;

private:
    static std::string generate_body(const driver_config_type &);
};

extern "C" [[maybe_unused]] IDriver *create() {
    return new CloudflareDriver;
}

#endif //YADDNSC_CLOUDFLARE_H
