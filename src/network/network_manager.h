//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_NETWORK_NETWORK_MANAGER_H
#define YADDNSC_NETWORK_NETWORK_MANAGER_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "mixin.h"

class NetworkManager {
public:
    NetworkManager();

    ~NetworkManager();

    [[nodiscard]] std::vector<std::string> get_interfaces() const;

    [[nodiscard]] std::map<std::string, int> get_interface_ip_addresses(const std::string &interface_name) const;

private:
    [[maybe_unused, no_unique_address]] NoCopy _nc_;
    [[maybe_unused, no_unique_address]] NoMove _nm_;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_NETWORK_NETWORK_MANAGER_H
