//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_NETWORK_MANAGER_H
#define YADDNSC_NETWORK_MANAGER_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base_classes.h"

class NetworkManager : public RestrictedClass {
public:
    NetworkManager();

    ~NetworkManager() override;

    [[nodiscard]] std::vector<std::string> get_interfaces() const;

    [[nodiscard]] std::map<std::string, int> get_interface_ip_addresses(const std::string &interface_name) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_NETWORK_MANAGER_H
