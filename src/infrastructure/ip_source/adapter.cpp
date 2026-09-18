//
// Created by Kotarou on 2026/9/17.
//

#include "adapter.h"

#include <exception>
#include <new>
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
        auto source = factory_(config);
        if (!source) {
            return std::unexpected(std::move(source.error()));
        }
        if (*source == nullptr) {
            return std::unexpected(
                domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, "IP source factory returned null"});
        }
        return (*source)->resolve(token);
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        return std::unexpected(domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, e.what()});
    } catch (...) {
        return std::unexpected(
            domain::IpSourceError{domain::IpSourceError::Code::UNKNOWN, "unknown non-standard exception"});
    }
}
