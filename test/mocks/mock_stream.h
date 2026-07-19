//
// MockStream — GoogleMock-based mock for the Transport::Stream interface.
//
// Allows tests to simulate transport-level I/O outcomes (partial reads,
// timeouts, cancellation, connection failure) without actual sockets
// or TLS connections.
//
// Supports both EXPECT_CALL-based expectations for fine-grained control
// and a simplified helper mode for common patterns.
//
// Helper mode (set_read_data / set_error):
//   MockStream stream;
//   stream.set_read_data({0x01, 0x02, 0x03});
//   stream.set_send_error(Transport::IoError::TIMEOUT);
//
//   // read_some succeeds N times, then fails:
//   stream.set_read_error(Transport::IoError::CANCELLED, /*ok_before_fail=*/2);
//
// EXPECT_CALL mode:
//   MockStream stream;
//   EXPECT_CALL(stream, read_some(_, _))
//       .WillOnce(Return(size_t{0}));
// =============================================================================

#ifndef YADDNSC_TEST_MOCKS_MOCK_STREAM_H
#define YADDNSC_TEST_MOCKS_MOCK_STREAM_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <gmock/gmock.h>

#include "network/transport/stream.h"

class MockStream : public Transport::Stream {
public:
    // ── EXPECT_CALL mode ──
    // NOTE: return types with commas (e.g. std::expected<size_t, IoError>)
    // MUST be wrapped in extra parentheses for the MOCK_METHOD macro.

    MOCK_METHOD((std::expected<size_t, Transport::IoError>),
                read_some,
                (std::span<std::uint8_t> buf,
                 const Utils::CancellationToken& cancel_token),
                (override));

    MOCK_METHOD((std::expected<void, Transport::IoError>),
                read_exact,
                (std::span<std::uint8_t> buf,
                 const Utils::CancellationToken& cancel_token),
                (override));

    MOCK_METHOD((std::expected<void, Transport::IoError>),
                send_all,
                (std::span<const std::uint8_t> data,
                 const Utils::CancellationToken& cancel_token),
                (override));

    // ── Convenience: set a single block of data to be read ──
    void set_read_data(std::vector<std::uint8_t> data);

    // ── Convenience: make send_all return a fixed error ──
    void set_send_error(Transport::IoError err);

    // ── Convenience: make read_some/read_exact return a fixed error ──
    // @param ok_before_fail  Number of successful reads before the error triggers.
    void set_read_error(Transport::IoError err, int ok_before_fail = 0);

    // ── Accessors ──
    bool was_sent() const noexcept { return sent_; }
    size_t bytes_read() const noexcept { return bytes_read_; }

private:
    bool sent_ = false;
    size_t bytes_read_ = 0;
    std::vector<std::uint8_t> read_data_;
    size_t read_pos_ = 0;
    int ok_before_fail_ = 0;
    std::optional<Transport::IoError> forced_send_error_;
    std::optional<Transport::IoError> forced_read_error_;
};

// ── Inline convenience method implementations ──

inline void MockStream::set_read_data(std::vector<std::uint8_t> data) {
    read_data_ = std::move(data);
    read_pos_ = 0;

    ON_CALL(*this, read_some(testing::_, testing::_))
        .WillByDefault([this](std::span<std::uint8_t> buf,
                              const Utils::CancellationToken&)
                           -> std::expected<size_t, Transport::IoError> {
            if (forced_read_error_.has_value()) {
                if (ok_before_fail_ > 0) {
                    --ok_before_fail_;
                } else {
                    return std::unexpected(*forced_read_error_);
                }
            }
            const size_t avail = read_data_.size() - read_pos_;
            if (avail == 0) return size_t{0};
            const size_t take = std::min(buf.size(), avail);
            std::copy_n(read_data_.begin() + static_cast<std::ptrdiff_t>(read_pos_),
                        take, buf.begin());
            read_pos_ += take;
            bytes_read_ += take;
            return take;
        });

    ON_CALL(*this, read_exact(testing::_, testing::_))
        .WillByDefault([this](std::span<std::uint8_t> buf,
                              const Utils::CancellationToken&)
                           -> std::expected<void, Transport::IoError> {
            if (forced_read_error_.has_value()) {
                if (ok_before_fail_ > 0) {
                    --ok_before_fail_;
                } else {
                    return std::unexpected(*forced_read_error_);
                }
            }
            const size_t avail = read_data_.size() - read_pos_;
            if (avail < buf.size()) {
                return std::unexpected(Transport::IoError::CONNECTION_FAILED);
            }
            std::copy_n(read_data_.begin() + static_cast<std::ptrdiff_t>(read_pos_),
                        buf.size(), buf.begin());
            read_pos_ += buf.size();
            bytes_read_ += buf.size();
            return {};
        });

    // Default send_all: succeed and mark sent_.
    ON_CALL(*this, send_all(testing::_, testing::_))
        .WillByDefault([this](std::span<const std::uint8_t>,
                              const Utils::CancellationToken&)
                           -> std::expected<void, Transport::IoError> {
            sent_ = true;
            return {};
        });
}

inline void MockStream::set_send_error(Transport::IoError err) {
    forced_send_error_ = err;
    ON_CALL(*this, send_all(testing::_, testing::_))
        .WillByDefault([this](std::span<const std::uint8_t>,
                              const Utils::CancellationToken&)
                           -> std::expected<void, Transport::IoError> {
            return std::unexpected(*forced_send_error_);
        });
}

inline void MockStream::set_read_error(Transport::IoError err, int ok_before_fail) {
    forced_read_error_ = err;
    ok_before_fail_ = ok_before_fail;

    // If set_read_data hasn't been called yet, install a default ON_CALL
    // that returns the error immediately.
    if (read_data_.empty()) {
        ON_CALL(*this, read_some(testing::_, testing::_))
            .WillByDefault([this](std::span<std::uint8_t>,
                                  const Utils::CancellationToken&)
                               -> std::expected<size_t, Transport::IoError> {
                return std::unexpected(*forced_read_error_);
            });

        ON_CALL(*this, read_exact(testing::_, testing::_))
            .WillByDefault([this](std::span<std::uint8_t>,
                                  const Utils::CancellationToken&)
                               -> std::expected<void, Transport::IoError> {
                return std::unexpected(*forced_read_error_);
            });
    }
}

#endif  // YADDNSC_TEST_MOCKS_MOCK_STREAM_H
