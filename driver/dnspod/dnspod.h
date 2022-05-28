//
// Created by Kotarou on 2022/4/11.
//

#ifndef YADDNSC_DNSPOD_H
#define YADDNSC_DNSPOD_H

#include "../base_driver.h"

class DNSPodDriver : public BaseDriver {
public:
    DNSPodDriver();

    [[nodiscard]] driver_request generate_request(const driver_config_type &config) const override;

    [[nodiscard]] bool check_response(std::string_view view) const override;

    [[nodiscard]] driver_detail get_detail() const override;
};

extern "C" [[maybe_unused]] IDriver *create() {
    return new DNSPodDriver;
}

#endif //YADDNSC_DNSPOD_H
