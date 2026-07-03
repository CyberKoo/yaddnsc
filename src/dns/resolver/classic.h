//
// Created by Kotarou on 2026/6/17.
//

#ifndef YADDNSC_DNS_CLASSIC_H
#define YADDNSC_DNS_CLASSIC_H

#include <memory>
#include <string>
#include <optional>

#include "dns/util.h"
#include "base.h"

class ClassicResolver final : public ResolverBase {
public:
    explicit ClassicResolver();

    explicit ClassicResolver(std::optional<DNS::Server> server);

    ~ClassicResolver() override;

    [[nodiscard]] std::vector<uint8_t> query(const std::string &host, DNS::Type type) const override;

    [[nodiscard]] std::string_view get_type() const noexcept override { return TYPE; }

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    static constexpr std::string_view TYPE = "Classic";
};

#endif // YADDNSC_DNS_CLASSIC_H
