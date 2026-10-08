//
// ip_source — the coroutine IP-source factory as an application port.
//

#ifndef YADDNSC_IP_SOURCE_ADAPTER_H
#define YADDNSC_IP_SOURCE_ADAPTER_H

#include <vector>

#include <expected>

#include "application/ports.h"
#include "domain/config/runtime_config.h"
#include "domain/error/error.h"
#include "domain/network/inet_address.h"
#include "infrastructure/coro/task.hpp"
#include "infrastructure/net/http/types.h"

namespace ipsource {

/// IpSourceAdapter — app::IpSourcePort over the coroutine IP sources.
///
/// It composes source construction and resolution without remapping recoverable
/// errors; only an unexpected implementation exception becomes Code::UNKNOWN
/// (allocation failure stays a defect). An empty candidate vector is a success.
///
/// Thread safety: resolve() is loop-thread only; the adapter holds only the
/// shared HTTP policy, copied per source.
class IpSourceAdapter final : public app::IpSourcePort {
public:
    explicit IpSourceAdapter(http::Options http_options);

    [[nodiscard]] coro::Task<std::expected<std::vector<InetAddress>, domain::IpSourceError>> resolve(
        const domain::SubdomainConfig& config) override;

private:
    http::Options options_;
};

}  // namespace ipsource

#endif  // YADDNSC_IP_SOURCE_ADAPTER_H
