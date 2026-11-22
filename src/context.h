//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_CONTEXT_H
#define YADDNSC_CONTEXT_H

#include <memory>
#include <string>
#include <condition_variable>
#include <atomic>
#include <mutex>

#include "base_classes.h"

class DriverManager;

class Context final : public RestrictedClass {
public:
    static Context &getInstance() {
        static Context instance;
        return instance;
    }

    ~Context() override;

    std::unique_ptr<DriverManager> driver_manager_;

    std::string config_path_{};

    std::atomic<bool> terminate_{false};

    std::mutex mutex_{};

    std::condition_variable condition_{};

private:
    Context();
};

#endif //YADDNSC_CONTEXT_H
