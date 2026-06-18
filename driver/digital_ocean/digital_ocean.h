//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H
#define YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H

#include <array>

#include <driver/base_driver.h>

class DigitalOceanDriver final : public BaseDriver {
public:
    ~DigitalOceanDriver() override = default;

    [[nodiscard]] driver_request generate_request(const driver_config_type &config) const override;

    [[nodiscard]] bool check_response(std::string_view response) const override;

    [[nodiscard]] DriverDetail get_detail() const override;

protected:
    [[nodiscard]] std::span<const std::string_view> get_required_params() const override {
        return required_params_;
    }

private:
    static constexpr std::array<std::string_view, 4> required_params_{
        "domain", "record_id", "token", "ip_addr"
    };
};

#endif //YADDNSC_DRV_DIGITALOCEAN_DIGITALOCEAN_H
