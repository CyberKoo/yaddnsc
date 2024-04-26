//
// Created by Kotarou on 2022/4/23.
//

#include "context.h"
#include "driver_manager.h"

Context::Context() : driver_manager_(std::make_unique<DriverManager>()) {
}

Context::~Context() = default;
