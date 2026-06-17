//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_APP_CONTEXT_H
#define YADDNSC_APP_CONTEXT_H

#include <memory>

class DriverManager;
class NetworkManager;

struct AppContext {
    std::unique_ptr<DriverManager> driver_manager_;
    std::unique_ptr<NetworkManager> network_manager_;
};

#endif //YADDNSC_APP_CONTEXT_H
