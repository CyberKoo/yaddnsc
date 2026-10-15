// The classic UDP exchange's connection_error() arm (dns/exchange.cpp) is only
// reachable when a socket syscall fails, and the exchange owns its sockets
// internally — there is no injection seam. Replacing socket(2) makes that
// failure deterministic and keeps the case off the network: the send never
// leaves the process, and nothing has to answer on the other end.
//
// Its own executable: the replacement is process-wide, so it must not share a
// binary with the suite that exercises real sockets.
//
// Requires "LINKER:--wrap=socket" and YADDNSC_TEST_WRAP_SOCKET on the test
// target (see test/unit/CMakeLists.txt); without them the binary carries no
// case and the fault injection is not built.
//

#ifdef YADDNSC_TEST_WRAP_SOCKET

#include <cerrno>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <expected>
#include <gtest/gtest.h>

#include "coro/coro.h"
#include "domain/error/dns_error.h"
#include "domain/error/dns_error_info.h"
#include "domain/network/inet_address.h"
#include "infrastructure/dns/exchange.h"
#include "infrastructure/dns/types.h"
#include "infrastructure/dns/wire/query_util.h"

namespace {

/// While engaged, every socket(2) in this process fails with this errno.
std::optional<int> forced_socket_errno{};

/// Scoped so a failing expectation — or an exception — cannot leave the
/// process-wide replacement engaged for the next, shuffled case.
class ForcedSocketFailure {
public:
    explicit ForcedSocketFailure(const int err) { forced_socket_errno = err; }

    ForcedSocketFailure(const ForcedSocketFailure&) = delete;
    ForcedSocketFailure& operator=(const ForcedSocketFailure&) = delete;

    ~ForcedSocketFailure() { forced_socket_errno.reset(); }
};

[[nodiscard]] domain::InetAddress server_address() {
    const auto address = domain::InetAddress::parse("203.0.113.10");
    EXPECT_TRUE(address.has_value());
    return address.value_or(domain::InetAddress{});
}

}  // namespace

// NOLINTNEXTLINE(bugprone-reserved-identifier)  // ld --wrap requires this exact symbol name
extern "C" int __real_socket(int domain, int type, int protocol);

// NOLINTNEXTLINE(bugprone-reserved-identifier)  // ld --wrap requires this exact symbol name
extern "C" int __wrap_socket(int domain, int type, int protocol) {
    if (forced_socket_errno.has_value()) {
        // EMFILE, not EINVAL: EINVAL is the SOCK_CLOEXEC probe that
        // open_socket() retries without the flag, which would open for real.
        errno = *forced_socket_errno;
        return -1;
    }
    return __real_socket(domain, type, protocol);
}

namespace {

TEST(ClassicExchangeSocketFailure, query_udp_SocketCreationFails_ReportsTheSendStageAsConnection) {
    const auto server = server_address();
    const auto query = dns::build_query("x.test", dns::RecordType::A);

    std::optional<std::expected<dns::detail::UdpAnswer, domain::DnsErrorInfo>> result;
    {
        const ForcedSocketFailure failure{EMFILE};
        result.emplace(coro::run(dns::detail::query_udp(server, 53, query)));
    }

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    // A local resource failure is CONNECTION: the exchange is worth retrying
    // against another backend, and it never became a verdict on the name.
    EXPECT_EQ(result->error().code, domain::DnsError::CONNECTION);
    EXPECT_EQ(result->error().message, "DNS UDP send to 203.0.113.10:53 failed");
}

TEST(ClassicExchangeSocketFailure, query_classic_SocketCreationFails_ReportsConnectionWithoutTcpFallback) {
    // No UDP answer means no TC bit, so the TCP fallback stays unreachable: the
    // attempt budget wraps a failure that already ended the exchange.
    const auto server = server_address();
    const auto query = dns::build_query("x.test", dns::RecordType::A);

    std::optional<std::expected<std::vector<std::uint8_t>, domain::DnsErrorInfo>> result;
    {
        const ForcedSocketFailure failure{EMFILE};
        result.emplace(coro::run(dns::detail::query_classic(server, 53, query)));
    }

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_EQ(result->error().code, domain::DnsError::CONNECTION);
    EXPECT_EQ(result->error().message, "DNS UDP send to 203.0.113.10:53 failed");
}

}  // namespace

#endif  // YADDNSC_TEST_WRAP_SOCKET