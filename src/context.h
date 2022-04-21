//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_CONTEXT_H
#define YADDNSC_CONTEXT_H

#include <memory>
#include <condition_variable>

#include "IDriver.h"
#include "non_copyable.h"
#include "driver_manager.h"

class Context : NonCopyable {
public:
    static Context &getInstance() {
        static Context instance;
        return instance;
    }

public:
    std::unique_ptr<DriverManager> driver_manager{};

    std::string config_path{};

    Config::resolver_config_t resolver_config{};

    bool terminate{false};

    std::condition_variable cv{};
private:
    Context() {
        driver_manager = std::make_unique<DriverManager>();
    };
};

#endif //YADDNSC_CONTEXT_H
