//
// Created by Kotarou on 2022/4/11.
//

#ifndef YADDNSC_DRV_DNSPOD_DNSPOD_H
#define YADDNSC_DRV_DNSPOD_DNSPOD_H

#include "driver/base_driver.h"

class DNSPodDriver final : public BaseDriver {
public:
    [[nodiscard]] driver_request generate_request(
        const driver_config_type &config, const UpdateContext &ctx) const override;

    [[nodiscard]] bool check_response(const HttpResponseData &response) const override;

    [[nodiscard]] DriverDetail get_detail() const override;

private:
    [[nodiscard]] static std::string_view describe_error_code(std::string_view code);
};

#endif //YADDNSC_DRV_DNSPOD_DNSPOD_H
