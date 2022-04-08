//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRIVER_MANAGER_H
#define YADDNSC_DRIVER_MANAGER_H

#include <map>
#include <memory>
#include <vector>

#include "common_fwd.h"
#include "non_copyable.h"

class DriverManager : public NonCopyable {
public:
    ~DriverManager();

    void load_driver(std::string_view);

    std::vector<std::string> get_loaded_drivers();

    std::unique_ptr<IDriver> &get_driver(std::string_view);

private:

    bool is_not_loaded(std::unique_ptr<IDriver> &) const;

    static void *open_file(std::string_view);

    static IDriver *get_instance(void *);

    static std::string_view get_driver_name(std::string_view path);

private:
    std::map<std::string, std::unique_ptr<IDriver>> _driver_map;

    std::vector<void *> _driver_handle;
};

#endif //YADDNSC_DRIVER_MANAGER_H
