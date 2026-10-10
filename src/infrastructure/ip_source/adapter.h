//
// ip_source — the coroutine IP-source factory as an application port.
//

#ifndef YADDNSC_INFRASTRUCTURE_IP_SOURCE_ADAPTER_H
#define YADDNSC_INFRASTRUCTURE_IP_SOURCE_ADAPTER_H

#include "application/ports/ip_source.h"
#include "infrastructure/http/types.h"

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

    [[nodiscard]] coro::Task<std::expected<std::vector<domain::InetAddress>, domain::IpSourceError>> resolve(
        const domain::SubdomainConfig& config) override;

private:
    http::Options options_;
};

}  // namespace ipsource

#endif  // YADDNSC_INFRASTRUCTURE_IP_SOURCE_ADAPTER_H
