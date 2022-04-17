//
// Created by Kotarou on 2022/4/11.
//

#ifndef YADDNSC_DNSPOD_H
#define YADDNSC_DNSPOD_H

#include "../driver.h"

class DNSPodDriver : public Driver {
public:
    DNSPodDriver();

    driver_request_t generate_request(const driver_config_t &config) override;

    bool check_response(std::string_view view) override;

    driver_detail_t get_detail() override;
};

extern "C" [[maybe_unused]] IDriver *create() {
    return new DNSPodDriver;
}

#endif //YADDNSC_DNSPOD_H
