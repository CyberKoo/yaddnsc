//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H
#define YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H

#include <array>

#include <driver/base_driver.h>

class CloudflareDriver final : public BaseDriver {
public:
    ~CloudflareDriver() override = default;

    [[nodiscard]] driver_request generate_request(const driver_config_type &) const override;

    [[nodiscard]] bool check_response(std::string_view) const override;

    [[nodiscard]] DriverDetail get_detail() const override;

protected:
    [[nodiscard]] std::span<const std::string_view> get_required_params() const override {
        return required_params_;
    }

private:
    static std::string generate_body(const driver_config_type &);

    static constexpr std::array<std::string_view, 6> required_params_{
        "sub_domain", "zone_id", "record_id", "token", "ip_addr", "rd_type"
    };
};

#endif //YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H
