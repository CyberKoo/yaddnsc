//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_CONTEXT_H
#define YADDNSC_CONTEXT_H

#include <memory>

class DriverManager;
class NetworkManager;

struct AppContext {
    std::unique_ptr<DriverManager> driver_manager_;
    std::unique_ptr<NetworkManager> network_manager_;
};

#endif //YADDNSC_CONTEXT_H
