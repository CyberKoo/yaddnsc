//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_NETWORK_MANAGER_H
#define YADDNSC_NETWORK_MANAGER_H

#include <map>
#include <memory>
#include <string>
#include <vector>

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    NetworkManager(const NetworkManager &) = delete;
    NetworkManager &operator=(const NetworkManager &) = delete;
    NetworkManager(NetworkManager &&) = delete;
    NetworkManager &operator=(NetworkManager &&) = delete;

    [[nodiscard]] std::vector<std::string> get_interfaces() const;

    [[nodiscard]] std::map<std::string, int> get_nif_ip_address(const std::string &nif) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif //YADDNSC_NETWORK_MANAGER_H
