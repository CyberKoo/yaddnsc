//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_SIMPLE_SIMPLE_H
#define YADDNSC_DRV_SIMPLE_SIMPLE_H

#include <driver/base_driver.h>

class SimpleDriver final : public BaseDriver {
public:
    ~SimpleDriver() override = default;

    [[nodiscard]] driver_request generate_request(const driver_config_type &, const UpdateContext &) const override;

    [[nodiscard]] DriverDetail get_detail() const override;

    [[nodiscard]] bool check_response(std::string_view) const override;
};

#endif //YADDNSC_DRV_SIMPLE_SIMPLE_H
