//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_SIMPLE_SIMPLE_H
#define YADDNSC_DRV_SIMPLE_SIMPLE_H

#include <array>

#include <driver/base_driver.h>

class SimpleDriver final : public BaseDriver {
public:
    ~SimpleDriver() override = default;

    [[nodiscard]] driver_request generate_request(const driver_config_type &config) const override;

    [[nodiscard]] driver_detail get_detail() const override;

    [[nodiscard]] bool check_response(std::string_view) const override;

protected:
    [[nodiscard]] std::span<const std::string_view> get_required_params() const override {
        return required_params_;
    }

private:
    static std::map<std::string, std::string> get_format_params(const driver_config_type &config);

    static constexpr std::array<std::string_view, 1> required_params_{"url"};
};

#endif //YADDNSC_DRV_SIMPLE_SIMPLE_H
