//
// Unit tests for Transport (src/network/transport/).
//
// Verifies (no network I/O):
//   - Options field defaults.
//   - Eager host validation in TlsStream / TcpStream constructors.
//   - detail::poll_fd cancellation semantics using a silent pipe (the same
//     primitive every stream I/O path is built on).
// =============================================================================

#include <chrono>
#include <thread>

#include <expected>
#include <gtest/gtest.h>
#include <poll.h>
#include <unistd.h>

#include "network/transport/detail/socket_stream.h"
#include "network/transport/io_error.h"
#include "network/transport/options.h"
#include "network/transport/tcp_stream.h"
#include "network/transport/tls_stream.h"
#include "util/cancellation_token.hpp"
#include "util/fd.hpp"

using namespace std::chrono_literals;
using Transport::IoError;

// ── Options defaults ─────────────────────────────────────────────────────────

TEST(NetTransportOptions, Defaults) {
    const Transport::Options opts;
    EXPECT_EQ(opts.connect_timeout, 5000ms);
    EXPECT_EQ(opts.read_timeout, 5000ms);
    EXPECT_EQ(opts.write_timeout, 5000ms);
    EXPECT_FALSE(opts.interface.has_value());
    EXPECT_FALSE(opts.address_family.has_value());

    const Transport::TlsOptions tls;
    EXPECT_TRUE(tls.verify_peer);
    EXPECT_FALSE(tls.sni_hostname.has_value());
    EXPECT_FALSE(tls.ca_bundle.has_value());
    EXPECT_TRUE(tls.alpn_proto.empty());
}

// ── Eager host validation ────────────────────────────────────────────────────

TEST(NetTransportCtor, TlsStream_RejectsInvalidHost) {
    const Utils::CancellationToken token;
    EXPECT_THROW((Transport::TlsStream("not_a_valid_address!!!", 443, {}, {}, token)), std::invalid_argument);
}

TEST(NetTransportCtor, TcpStream_RejectsInvalidHost) {
    const Utils::CancellationToken token;
    EXPECT_THROW((Transport::TcpStream("not_a_valid_address!!!", 80, {}, token)), std::invalid_argument);
}

TEST(NetTransportCtor, AcceptsIpLiteralAndDomain) {
    const Utils::CancellationToken token;
    EXPECT_NO_THROW((Transport::TlsStream("127.0.0.1", 443, {}, {}, token)));
    EXPECT_NO_THROW((Transport::TlsStream("dns.example.com", 443, {}, {}, token)));
    EXPECT_NO_THROW((Transport::TcpStream("::1", 80, {}, token)));
}

// ── poll_fd cancellation semantics ───────────────────────────────────────────

namespace {

/// A pipe whose read end never becomes ready (silent writer end kept open).
struct SilentFd {
    SilentFd() {
        auto [r, w] = Utils::make_pipe();
        read = std::move(r);
        write = std::move(w);
    }

    Utils::UniqueFd read;
    Utils::UniqueFd write;
};

}  // namespace

TEST(NetTransportPollFd, SilentFd_TimesOut) {
    const SilentFd silent;
    const Utils::CancellationToken token;

    const auto result = Transport::detail::poll_fd(silent.read.get(), POLLIN, 20ms, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::TIMEOUT);
}

TEST(NetTransportPollFd, TriggeredToken_ReturnsCancelledImmediately) {
    const SilentFd silent;
    Utils::CancellationSource source;
    source.trigger();
    const auto token = source.token();

    const auto result = Transport::detail::poll_fd(silent.read.get(), POLLIN, 5000ms, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CANCELLED);
}

TEST(NetTransportPollFd, TriggerFromAnotherThread_WakesPoll) {
    const SilentFd silent;
    Utils::CancellationSource source;
    const auto token = source.token();

    std::jthread triggerrer([src = source] {
        std::this_thread::sleep_for(30ms);
        src.trigger();
    });

    const auto start = std::chrono::steady_clock::now();
    const auto result = Transport::detail::poll_fd(silent.read.get(), POLLIN, 5000ms, token);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CANCELLED);
    EXPECT_LT(elapsed, 2s);
}

TEST(NetTransportPollFd, DrainedSignal_StillCancelledViaLatch) {
    const SilentFd silent;
    Utils::CancellationSource source;
    const auto token = source.token();

    source.trigger();
    token.drain();  // another consumer drains the pipe edge...

    // ...the latched flag must still cancel the operation.
    const auto result = Transport::detail::poll_fd(silent.read.get(), POLLIN, 5000ms, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), IoError::CANCELLED);
}

TEST(NetTransportPollFd, ReadyFd_ReturnsOk) {
    auto [read_end, write_end] = Utils::make_pipe();
    const Utils::CancellationToken token;

    const char c = 'x';
    ASSERT_EQ(::write(write_end.get(), &c, 1), 1);

    const auto result = Transport::detail::poll_fd(read_end.get(), POLLIN, 100ms, token);
    EXPECT_TRUE(result);
}
