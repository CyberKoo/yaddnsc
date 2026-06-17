//
// Created by kotarou on 2026/6/17.
//

#ifndef YADDNSC_DRV_DIGITALOCEAN_RESPONSE_H
#define YADDNSC_DRV_DIGITALOCEAN_RESPONSE_H

#include <string>
#include <optional>

struct DigitalOceanResponse {
    std::optional<std::string> message;
};

#endif //YADDNSC_DRV_DIGITALOCEAN_RESPONSE_H
