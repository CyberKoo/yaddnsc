//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_IP_SOURCE_ADAPTER_H
#define YADDNSC_IP_SOURCE_ADAPTER_H

#include <functional>
#include <memory>

#include "application/ports/ip_source.h"
#include "domain/config/runtime_config.h"
#include "infrastructure/ip_source/factory.h"

namespace Utils {
class CancellationToken;
}  // namespace Utils

/// IpSourceAdapter — IpSourcePort implementation over structured source
/// results. It composes factory creation and source resolution without
/// remapping recoverable errors; only an unexpected implementation exception
/// becomes Code::UNKNOWN. An empty candidate vector remains successful.
///
/// @note Thread-safe: resolve() is const and owns no mutable state.
class IpSourceAdapter final : public IpSourcePort {
public:
    /// Factory type for creating IP source instances (tests may inject stubs).
    using FactoryFn = std::function<IpSourceFactory::Result(const domain::SubdomainConfig&)>;

    /// @param factory  Source factory; defaults to IpSourceFactory::create.
    explicit IpSourceAdapter(FactoryFn factory = {});

    [[nodiscard]] std::expected<std::vector<InetAddress>, domain::IpSourceError> resolve(
        const domain::SubdomainConfig& config, const Utils::CancellationToken& token) const override;

private:
    FactoryFn factory_;
};

#endif  // YADDNSC_IP_SOURCE_ADAPTER_H
