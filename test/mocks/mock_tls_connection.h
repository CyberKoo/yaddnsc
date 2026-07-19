//
// MockTlsConnection — GoogleMock-based mock for TlsConnectionBase.
//
// Allows tests to simulate TLS connection outcomes (successful handshake,
// timeout, cancellation, connection failure, ALPN negotiation, etc.)
// without real OpenSSL I/O.
//
// Usage (EXPECT_CALL mode):
//   MockTlsConnection mock;
//   EXPECT_CALL(mock, connect())
//       .WillOnce(Return(std::unexpected(TlsConnectionBase::IoStatus::TIMEOUT)));
//
// Usage (convenience mode):
//   MockTlsConnection mock;
//   mock.set_connect_result({});                           // success
//   mock.set_send_result(std::unexpected(IoStatus::CANCELLED));  // fail send
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_MOCK_TLS_CONNECTION_H
#define YADDNSC_TEST_MOCKS_MOCK_TLS_CONNECTION_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include "network/tls_connection.h"

class MockTlsConnection : public TlsConnectionBase {
public:
    using IoStatus = TlsConnectionBase::IoStatus;

    // ── EXPECT_CALL mode ──
    // NOTE: Return types with commas must be wrapped in extra parentheses.

    MOCK_METHOD((std::expected<void, IoStatus>), connect, (), (override));
    MOCK_METHOD(void, close, (), (noexcept, override));
    MOCK_METHOD(bool, is_connected, (), (const, noexcept, override));
    MOCK_METHOD(bool, is_healthy, (), (const, noexcept, override));

    MOCK_METHOD((std::expected<void, IoStatus>), send_all,
                (std::span<const std::uint8_t> data,
                 const Utils::CancellationToken& cancel_token),
                (override));

    MOCK_METHOD((std::expected<void, IoStatus>), read_exact,
                (std::span<std::uint8_t> buf,
                 const Utils::CancellationToken& cancel_token),
                (override));

    MOCK_METHOD((std::expected<size_t, IoStatus>), read_some,
                (std::span<std::uint8_t> buf,
                 const Utils::CancellationToken& cancel_token),
                (override));

    MOCK_METHOD((std::expected<void, IoStatus>), shutdown, (), (override));
    MOCK_METHOD(std::string, negotiated_alpn, (), (const, noexcept, override));
    MOCK_METHOD(void, set_sni_hostname, (std::string hostname), (override));

    // ── Convenience: configure default behaviours ──

    void set_connect_result(std::expected<void, IoStatus> result) {
        ON_CALL(*this, connect()).WillByDefault(testing::Return(result));
    }

    void set_send_result(std::expected<void, IoStatus> result) {
        ON_CALL(*this, send_all(testing::_, testing::_))
            .WillByDefault(testing::Return(result));
    }

    void set_read_data(std::vector<std::uint8_t> data) {
        read_data_ = std::move(data);
        read_pos_ = 0;
        ON_CALL(*this, read_some(testing::_, testing::_))
            .WillByDefault([this](std::span<std::uint8_t> buf,
                                  const Utils::CancellationToken&)
                               -> std::expected<size_t, IoStatus> {
                const size_t avail = read_data_.size() - read_pos_;
                if (avail == 0) return size_t{0};
                const size_t take = std::min(buf.size(), avail);
                std::copy_n(read_data_.begin() + static_cast<std::ptrdiff_t>(read_pos_),
                            take, buf.begin());
                read_pos_ += take;
                return take;
            });
        ON_CALL(*this, read_exact(testing::_, testing::_))
            .WillByDefault([this](std::span<std::uint8_t> buf,
                                  const Utils::CancellationToken&)
                               -> std::expected<void, IoStatus> {
                const size_t avail = read_data_.size() - read_pos_;
                if (avail < buf.size()) {
                    return std::unexpected(IoStatus::ERROR);
                }
                std::copy_n(read_data_.begin() + static_cast<std::ptrdiff_t>(read_pos_),
                            buf.size(), buf.begin());
                read_pos_ += buf.size();
                return {};
            });
    }

private:
    std::vector<std::uint8_t> read_data_;
    size_t read_pos_ = 0;
};

#endif  // YADDNSC_TEST_MOCKS_MOCK_TLS_CONNECTION_H
