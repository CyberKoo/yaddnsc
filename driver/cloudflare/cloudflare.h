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

    [[nodiscard]] driver_request_t generate_request(const driver_config_t &) const override;

    [[nodiscard]] bool check_response(std::string_view) const override;

    [[nodiscard]] constexpr driver_detail_t get_detail() const override;

private:
    static std::string generate_body(const driver_config_t &);
};

extern "C" [[maybe_unused]] IDriver *create() {
    return new CloudflareDriver;
}

#endif //YADDNSC_CLOUDFLARE_H
