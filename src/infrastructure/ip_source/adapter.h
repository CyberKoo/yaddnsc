//
// Created by Kotarou on 2026/9/17.
//

#ifndef YADDNSC_IP_SOURCE_ADAPTER_H
#define YADDNSC_IP_SOURCE_ADAPTER_H

#include <functional>
#include <memory>

#include "application/ports/ip_source.h"
#include "domain/config/runtime_config.h"

class IpSourceBase;

namespace Utils {
class CancellationToken;
}  // namespace Utils

/// IpSourceAdapter — IpSourcePort implementation over the legacy
/// IpSourceFactory + IpSourceBase (throwing) stack.
///
/// Translates the legacy exception contract into error values: any
/// std::exception escaping the factory or resolve() becomes
/// {Code::UNAVAILABLE, e.what()} — the workflow keeps its uniform
/// "log and skip this cycle" handling. An empty candidate vector passes
/// through unchanged (success, not an error).
///
/// @note Thread-safe: resolve() is const and owns no mutable state.
class IpSourceAdapter final : public IpSourcePort {
public:
    /// Factory type for creating IP source instances (tests may inject stubs).
    using FactoryFn = std::function<std::unique_ptr<IpSourceBase>(const domain::SubdomainConfig&)>;

    /// @param factory  Source factory; defaults to IpSourceFactory::create.
    explicit IpSourceAdapter(FactoryFn factory = {});

    [[nodiscard]] std::expected<std::vector<InetAddress>, domain::IpSourceError> resolve(
        const domain::SubdomainConfig& config, const Utils::CancellationToken& token) const override;

private:
    FactoryFn factory_;
};

#endif  // YADDNSC_IP_SOURCE_ADAPTER_H
