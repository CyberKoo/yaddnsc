//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_CONTEXT_H
#define YADDNSC_CONTEXT_H

#include <memory>
#include <condition_variable>

#include "base_classes.h"

class DriverManager;

class Context : public RestrictedClass {
private:
    struct DriverManagerDeleter {
        void operator()(DriverManager *);
    };

public:
    static Context &getInstance() {
        static Context instance;
        return instance;
    }

    std::unique_ptr<DriverManager, DriverManagerDeleter> driver_manager_{};

    std::string config_path_{};

    bool terminate_{false};

    std::condition_variable condition_{};
private:
    Context();
};

#endif //YADDNSC_CONTEXT_H
