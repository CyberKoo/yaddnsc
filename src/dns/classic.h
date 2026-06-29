//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_CLASSIC_H
#define YADDNSC_DNS_CLASSIC_H

#include <memory>
#include <string>
#include <optional>

#include "base.h"
#include "type.h"

class ClassicResolver final : public ResolverBase {
public:
    explicit ClassicResolver();

    explicit ClassicResolver(std::optional<dns_server_type> server);

    ~ClassicResolver() override;

    [[nodiscard]] std::vector<uint8_t> query(const std::string &host, dns_type type) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif // YADDNSC_DNS_SYSTEM_H
