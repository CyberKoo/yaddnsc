//
// Created by Kotarou on 2022/4/11.
//

#ifndef YADDNSC_DRV_DNSPOD_DNSPOD_H
#define YADDNSC_DRV_DNSPOD_DNSPOD_H

#include <array>

#include <driver/base_driver.h>

class DNSPodDriver final : public BaseDriver {
public:
    [[nodiscard]] driver_request generate_request(const driver_config_type &config) const override;

    [[nodiscard]] bool check_response(std::string_view view) const override;

    [[nodiscard]] driver_detail get_detail() const override;

protected:
    [[nodiscard]] std::span<const std::string_view> get_required_params() const override {
        return required_params_;
    }

private:
    [[nodiscard]] static std::string_view describe_error_code(std::string_view code);

    static constexpr std::array<std::string_view, 5> required_params_{
        "domain_id", "record_id", "subdomain", "login_token", "ip_addr"
    };
};

#endif //YADDNSC_DRV_DNSPOD_DNSPOD_H
