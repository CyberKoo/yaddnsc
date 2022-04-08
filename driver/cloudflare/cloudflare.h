//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_CLOUDFLARE_H
#define YADDNSC_CLOUDFLARE_H

#include "../driver.h"

class CloudflareDriver final : public Driver {
public:
    CloudflareDriver();

    ~CloudflareDriver() override = default;

    request_t generate_request(const driver_config_t &config) override;

    bool check_response(std::string_view) override;

    driver_detail_t get_detail() override;
};

extern "C" [[maybe_unused]] IDriver *create() {
    return new CloudflareDriver;
}

#endif //YADDNSC_CLOUDFLARE_H
