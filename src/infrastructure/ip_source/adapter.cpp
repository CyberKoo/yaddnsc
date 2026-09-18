//
// Created by Kotarou on 2026/9/17.
//

#include "adapter.h"

#include <exception>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <expected>

#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "infrastructure/ip_source/base.h"
#include "support/util/cancellation_token.hpp"

#include "factory.h"
#include "support/util/cancellation_token.hpp"

IpSourceAdapter::IpSourceAdapter(FactoryFn factory)
    : factory_(factory ? std::move(factory) : FactoryFn([](const domain::SubdomainConfig& cfg) {
          return IpSourceFactory::create(cfg);
      })) {}

std::expected<std::vector<InetAddress>, domain::IpSourceError> IpSourceAdapter::resolve(
    const domain::SubdomainConfig& config, const Utils::CancellationToken& token) const {
    if (token.is_triggered()) {
        return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::CANCELLED, "IP source lookup cancelled"});
    }

    try {
        return factory_(config)->resolve(token);
    } catch (const std::exception& e) {
        return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNAVAILABLE, e.what()});
    } catch (...) {
        return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, "unknown non-standard exception"});
    }
}
