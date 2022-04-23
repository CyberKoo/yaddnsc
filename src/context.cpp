//
// Created by Kotarou on 2022/4/23.
//

#include "context.h"
#include "IDriver.h"
#include "driver_manager.h"

void Context::DriverManagerDeleter::operator()(DriverManager *ptr) {
    delete ptr;
}

Context::Context() : driver_manager(new DriverManager) {

}
