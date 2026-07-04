//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H
#define YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H

#include "config.hpp"
#include "driver/base.h"

class CloudflareDriver final : public BaseDriver {
public:
    ~CloudflareDriver() override = default;

    [[nodiscard]] DriverRequest generate_request(const DriverConfig &config, const UpdateContext &ctx) const override;

    [[nodiscard]] bool check_response(const HttpResponse &response) const override;

    [[nodiscard]] DriverDetail get_detail() const override;

private:
    static std::string generate_body(const CloudflareParams &cfg, const UpdateContext &ctx);
};

#endif //YADDNSC_DRV_CLOUDFLARE_CLOUDFLARE_H
