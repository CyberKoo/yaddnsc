//
// Production TLS retry direction.
//
// read_ssl / write_ssl wait for the poll event SSL_get_error reports.
// A custom BIO forces one WANT_* until that event is actually ready on a
// connected socket. The other event stays unready for the whole budget, so
// a swapped wait runs until the deadline and a matching wait returns when
// the delayed event arrives.
//

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

#include <gtest/gtest.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>

#include "domain/network/inet_address.h"
#include "infrastructure/network/socket.h"
#include "infrastructure/network/socket_addr.h"
#include "infrastructure/network/transport/detail/tls_io.h"
#include "infrastructure/network/transport/io_error.h"
#include "infrastructure/network/transport/tls_stream.h"
#include "support/util/cancellation_token.hpp"

using namespace std::chrono_literals;

namespace {

constexpr auto DIRECTION_DELAY = 100ms;
constexpr auto WINDOW_SETTLE = 60ms;
constexpr auto OPERATION_BUDGET = 1500ms;
constexpr auto MIN_BLOCKED = 50ms;
constexpr auto MAX_BLOCKED = 900ms;
constexpr int WINDOW_CLAMP = 1024;

struct DirectionProbe {
    int fd = -1;
    short ready_event = POLLOUT;
    bool force_write = true;
};

struct JoinedThread {
    explicit JoinedThread(std::thread worker) : thread(std::move(worker)) {}

    ~JoinedThread() {
        if (thread.joinable()) {
            thread.join();
        }
    }

    JoinedThread(const JoinedThread&) = delete;
    JoinedThread& operator=(const JoinedThread&) = delete;

    std::thread thread;
};

int bio_transfer(BIO* bio) {
    auto* probe = static_cast<DirectionProbe*>(BIO_get_data(bio));
    BIO_clear_retry_flags(bio);
    if (probe == nullptr || probe->fd < 0) {
        return -1;
    }
    pollfd pfd{.fd = probe->fd, .events = probe->ready_event, .revents = 0};
    const int ready = ::poll(&pfd, 1, 0);
    if (ready > 0 && (pfd.revents & probe->ready_event) != 0) {
        return -1;
    }
    if (probe->force_write) {
        BIO_set_retry_write(bio);
    } else {
        BIO_set_retry_read(bio);
    }
    return -1;
}

int bio_read(BIO* bio, char*, int) {
    return bio_transfer(bio);
}

int bio_write(BIO* bio, const char*, int) {
    return bio_transfer(bio);
}

long bio_ctrl(BIO*, int cmd, long, void*) {
    if (cmd == BIO_CTRL_FLUSH) {
        return 1;
    }
    return 0;
}

int bio_create(BIO* bio) {
    BIO_set_init(bio, 1);
    return 1;
}

int bio_destroy(BIO*) {
    return 1;
}

[[nodiscard]] BIO_METHOD* direction_method() {
    static BIO_METHOD* method = nullptr;
    if (method == nullptr) {
        method = BIO_meth_new(BIO_get_new_index(), "yaddnsc-tls-direction");
        if (method == nullptr) {
            return nullptr;
        }
        BIO_meth_set_read(method, bio_read);
        BIO_meth_set_write(method, bio_write);
        BIO_meth_set_ctrl(method, bio_ctrl);
        BIO_meth_set_create(method, bio_create);
        BIO_meth_set_destroy(method, bio_destroy);
    }
    return method;
}

[[nodiscard]] bool event_ready(const int fd, const short event) {
    pollfd pfd{.fd = fd, .events = event, .revents = 0};
    const int ready = ::poll(&pfd, 1, 0);
    return ready > 0 && (pfd.revents & event) != 0;
}

/// Bytes queued before the local send buffer or the peer window refuses more.
[[nodiscard]] std::optional<std::size_t> fill_send_buffer(Socket& client) {
    std::array<std::byte, 4096> chunk{};
    std::size_t queued = 0;
    constexpr std::size_t LIMIT = 8U * 1024U * 1024U;
    while (queued < LIMIT) {
        auto n = client.send_some(chunk);
        if (!n) {
            if (n.error() == EAGAIN || n.error() == EWOULDBLOCK) {
                return queued;
            }
            return std::nullopt;
        }
        if (*n == 0) {
            return std::nullopt;
        }
        queued += *n;
    }
    return std::nullopt;
}

/// Keep the peer from advertising a window large enough for the delayed ACK
/// to make the client writable again. The client send buffer stays large, so
/// EAGAIN means the peer window is closed.
[[nodiscard]] bool clamp_receive_window(Socket& sock) {
    return sock.set_option(SOL_SOCKET, SO_RCVBUF, WINDOW_CLAMP).has_value() &&
           sock.set_option(IPPROTO_TCP, TCP_WINDOW_CLAMP, WINDOW_CLAMP).has_value();
}

struct SocketPair {
    Socket client;
    Socket peer;
};

[[nodiscard]] std::optional<SocketPair> connected_pair() {
    auto ip = InetAddress::parse("127.0.0.1");
    if (!ip) {
        ADD_FAILURE() << "loopback address";
        return std::nullopt;
    }
    auto wildcard = SocketAddr::from_inet(*ip, 0);
    if (!wildcard) {
        ADD_FAILURE() << "socket address";
        return std::nullopt;
    }

    auto listener = Socket::open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    auto client = Socket::open(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!listener || !client) {
        ADD_FAILURE() << "socket";
        return std::nullopt;
    }
    const int send_buffer = 1024 * 1024;
    if (!clamp_receive_window(*listener) || !client->set_option(SOL_SOCKET, SO_SNDBUF, send_buffer)) {
        ADD_FAILURE() << "buffer size";
        return std::nullopt;
    }
    if (!listener->set_reuseaddr(true) || !listener->bind(*wildcard) || !listener->listen(1)) {
        ADD_FAILURE() << "listen";
        return std::nullopt;
    }
    auto bound = listener->get_sockname();
    if (!bound) {
        ADD_FAILURE() << "sockname";
        return std::nullopt;
    }
    const int nodelay = 1;
    (void) client->set_option(IPPROTO_TCP, TCP_NODELAY, nodelay);
    if (!client->connect(*bound, std::chrono::steady_clock::now() + 2s, {})) {
        ADD_FAILURE() << "connect";
        return std::nullopt;
    }
    auto peer = listener->accept();
    if (!peer) {
        ADD_FAILURE() << "accept";
        return std::nullopt;
    }
    if (!clamp_receive_window(*peer)) {
        ADD_FAILURE() << "peer buffer size";
        return std::nullopt;
    }
    (void) peer->set_option(IPPROTO_TCP, TCP_NODELAY, nodelay);
    return SocketPair{std::move(*client), std::move(*peer)};
}

struct TlsSession {
    Transport::SslCtxPtr ctx;
    Transport::SslPtr ssl;
};

[[nodiscard]] TlsSession open_session(DirectionProbe& probe) {
    TlsSession session;
    session.ctx.reset(SSL_CTX_new(TLS_client_method()));
    if (!session.ctx) {
        ADD_FAILURE() << "SSL_CTX_new";
        return session;
    }
    session.ssl.reset(SSL_new(session.ctx.get()));
    if (!session.ssl) {
        ADD_FAILURE() << "SSL_new";
        return session;
    }
    BIO_METHOD* method = direction_method();
    if (method == nullptr) {
        ADD_FAILURE() << "BIO_meth_new";
        return session;
    }
    BIO* bio = BIO_new(method);
    if (bio == nullptr) {
        ADD_FAILURE() << "BIO_new";
        return session;
    }
    BIO_set_data(bio, &probe);
    BIO_set_init(bio, 1);
    SSL_set_bio(session.ssl.get(), bio, bio);
    SSL_set_connect_state(session.ssl.get());
    return session;
}

void expect_wait_matches_direction(const bool timed_out, const std::chrono::steady_clock::duration elapsed) {
    EXPECT_FALSE(timed_out);
    EXPECT_GE(elapsed, MIN_BLOCKED);
    EXPECT_LT(elapsed, MAX_BLOCKED);
}

}  // namespace

TEST(TlsDirection, ReadWantWrite_WaitsForPollout) {
    auto pair = connected_pair();
    ASSERT_TRUE(pair.has_value());
    const auto queued = fill_send_buffer(pair->client);
    ASSERT_TRUE(queued.has_value()) << "send buffer did not fill";
    ASSERT_GT(*queued, 0u);
    // A delayed ACK can free the local send buffer. The peer window has to
    // still be closed after that, or this test is not observing POLLOUT.
    std::this_thread::sleep_for(WINDOW_SETTLE);
    ASSERT_FALSE(event_ready(pair->client.native_handle(), POLLOUT)) << "queued " << *queued;
    ASSERT_FALSE(event_ready(pair->client.native_handle(), POLLIN));

    // POLLOUT becomes ready only after the peer reads. POLLIN stays quiet,
    // so a WANT_WRITE handled as POLLIN consumes the whole budget.
    DirectionProbe probe{.fd = pair->client.native_handle(), .ready_event = POLLOUT, .force_write = true};
    auto session = open_session(probe);
    ASSERT_TRUE(session.ssl);
    // One small read is refilled from the queued send buffer before poll
    // observes it. Drain until the peer goes idle so the client becomes
    // writable and stays writable.
    JoinedThread peer{std::thread([&pair] {
        std::this_thread::sleep_for(DIRECTION_DELAY);
        (void) pair->peer.set_nonblocking(true);
        std::array<std::byte, 8192> scratch{};
        const auto give_up = std::chrono::steady_clock::now() + 500ms;
        while (std::chrono::steady_clock::now() < give_up) {
            pollfd pfd{.fd = pair->peer.native_handle(), .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, 20) <= 0) {
                break;
            }
            auto n = pair->peer.recv_some(scratch);
            if (!n || *n == 0) {
                break;
            }
        }
    })};

    std::array<std::uint8_t, 16> buf{};
    const auto start = std::chrono::steady_clock::now();
    const auto result = Transport::detail::read_ssl(session.ssl.get(), pair->client, buf, start + OPERATION_BUDGET, {});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const bool timed_out = !result.has_value() && result.error() == Transport::IoError::TIMEOUT;
    expect_wait_matches_direction(timed_out, elapsed);
}

TEST(TlsDirection, WriteWantRead_WaitsForPollin) {
    auto pair = connected_pair();
    ASSERT_TRUE(pair.has_value());
    ASSERT_TRUE(fill_send_buffer(pair->client));
    ASSERT_FALSE(event_ready(pair->client.native_handle(), POLLOUT));
    ASSERT_FALSE(event_ready(pair->client.native_handle(), POLLIN));

    // POLLIN becomes ready only after the peer writes. The send buffer stays
    // full, so a WANT_READ handled as POLLOUT consumes the whole budget.
    DirectionProbe probe{.fd = pair->client.native_handle(), .ready_event = POLLIN, .force_write = false};
    auto session = open_session(probe);
    ASSERT_TRUE(session.ssl);
    JoinedThread peer{std::thread([&pair] {
        std::this_thread::sleep_for(DIRECTION_DELAY);
        const auto byte = std::array{std::byte{0x1}};
        (void) pair->peer.send_some(byte);
    })};

    const auto byte = std::array<std::uint8_t, 1>{0x2};
    const auto start = std::chrono::steady_clock::now();
    const auto result =
        Transport::detail::write_ssl(session.ssl.get(), pair->client, byte, start + OPERATION_BUDGET, {});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const bool timed_out = !result.has_value() && result.error() == Transport::IoError::TIMEOUT;
    expect_wait_matches_direction(timed_out, elapsed);
}
