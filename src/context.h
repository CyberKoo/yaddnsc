//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_CONTEXT_H
#define YADDNSC_CONTEXT_H

#include <memory>
#include <condition_variable>

#include "config.h"
#include "non_copyable.h"

class DriverManager;

class Context : NonCopyable {
private:
    struct DriverManagerDeleter {
        void operator()(DriverManager *);
    };

public:
    static Context &getInstance() {
        static Context instance;
        return instance;
    }

    std::unique_ptr<DriverManager, DriverManagerDeleter> driver_manager{};

    std::string config_path{};

    Config::resolver_config_t resolver_config{};

    bool terminate{false};

    std::condition_variable cv{};
private:
    Context();
};

#endif //YADDNSC_CONTEXT_H
