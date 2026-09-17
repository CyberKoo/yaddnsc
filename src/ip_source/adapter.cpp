//
// Created by Kotarou on 2026/9/17.
//

#include "adapter.h"

#include <exception>
#include <utility>

#include "factory.h"
#include "base.h"

IpSourceAdapter::IpSourceAdapter(Utils::CancellationToken token, FactoryFn factory)
    : factory_(factory
                   ? std::move(factory)
                   : FactoryFn([token = std::move(token)](const domain::SubdomainConfig &cfg) {
                         return IpSourceFactory::create(cfg, token);
                     })) {
}

std::expected<std::vector<InetAddress>, domain::IpSourceError>
IpSourceAdapter::resolve(const domain::SubdomainConfig &config) const {
    try {
        return factory_(config)->resolve();
    } catch (const std::exception &e) {
        return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE, e.what()});
    } catch (...) {
        return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, "unknown non-standard exception"});
    }
}
