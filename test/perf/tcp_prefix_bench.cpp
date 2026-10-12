// Buffered DNS-sized length prefixes through the real TCP stream. Setup is
// outside the timed iterations; batching amortizes fresh-loop construction.
#include <array>
#include <cstdint>
#include <utility>

#include <benchmark/benchmark.h>
#include <fcntl.h>
#include <sys/socket.h>

#include "coro/coro.h"
#include "domain/network/inet_address.h"
#include "infrastructure/network/transport/socket_ops.h"
#include "infrastructure/network/transport/tcp_stream.h"
#include "support/util/fd.hpp"

namespace {

constexpr int PREFIXES_PER_BATCH = 1024;
constexpr std::array<std::uint8_t, 2> PREFIX{0, 42};

coro::Task<bool> transfer_prefixes(net::TcpStream& stream, const int peer) {
    std::array<std::uint8_t, 2> received{};
    for (int i = 0; i < PREFIXES_PER_BATCH; ++i) {
        if (!(co_await net::detail::send_all(peer, PREFIX)) || !(co_await stream.read_exact(received))) {
            co_return false;
        }
        benchmark::DoNotOptimize(received);
        if (received != PREFIX) {
            co_return false;
        }
    }
    co_return true;
}

void buffered_tcp_prefix(benchmark::State& state) {
    const auto address = domain::InetAddress::parse("127.0.0.1").value();
    auto opened = net::detail::open_tcp_socket(address);
    if (!opened) {
        state.SkipWithError("Cannot open listener");
        return;
    }
    const Utils::UniqueFd listener{std::move(*opened)};
    if (!net::detail::bind_local(listener.get(), address, 0) || ::listen(listener.get(), 1) != 0) {
        state.SkipWithError("Cannot bind listener");
        return;
    }
    const auto port = net::detail::local_port(listener.get());
    if (!port) {
        state.SkipWithError("Cannot read listener port");
        return;
    }
    net::TcpStream stream{address, *port};
    if (!coro::run(stream.ensure_connected())) {
        state.SkipWithError("Cannot connect stream");
        return;
    }
    const Utils::UniqueFd peer{::accept(listener.get(), nullptr, nullptr)};
    if (!peer) {
        state.SkipWithError("Cannot accept stream");
        return;
    }
    const int flags = ::fcntl(peer.get(), F_GETFL, 0);
    if (flags < 0 || ::fcntl(peer.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
        state.SkipWithError("Cannot make peer nonblocking");
        return;
    }
    for (auto _ : state) {
        if (!coro::run(transfer_prefixes(stream, peer.get()))) {
            state.SkipWithError("Prefix transfer failed");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations() * PREFIXES_PER_BATCH);
}

BENCHMARK(buffered_tcp_prefix);

}  // namespace
