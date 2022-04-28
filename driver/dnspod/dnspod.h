//
// Created by Kotarou on 2022/4/11.
//

#ifndef YADDNSC_DNSPOD_H
#define YADDNSC_DNSPOD_H

#include "../driver.h"

class DNSPodDriver : public Driver {
public:
    DNSPodDriver();

    [[nodiscard]] driver_request_t generate_request(const driver_config_t &config) const override;

    [[nodiscard]] bool check_response(std::string_view view) const override;

    [[nodiscard]] driver_detail_t get_detail() const override;
};

extern "C" [[maybe_unused]] IDriver *create() {
    return new DNSPodDriver;
}

#endif //YADDNSC_DNSPOD_H
