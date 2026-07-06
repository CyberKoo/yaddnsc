//
// Created by Kotarou on 2022/4/11.
//

#ifndef YADDNSC_DRV_DNSPOD_DNSPOD_H
#define YADDNSC_DRV_DNSPOD_DNSPOD_H

#include "driver/base.h"

class DNSPodDriver final : public BaseDriver {
public:
    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig &config, const DriverUpdateParams &ctx) const override;

    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    [[nodiscard]] DriverDetail get_detail() const override;

private:
    [[nodiscard]] static std::string_view describe_error_code(std::string_view code);
};

#endif //YADDNSC_DRV_DNSPOD_DNSPOD_H
