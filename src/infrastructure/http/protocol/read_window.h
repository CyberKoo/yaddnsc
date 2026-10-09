//
// http — sliding read window used by the response decoders.
//
// Chunked decoding consumes the accumulated buffer in tiny increments: a chunk
// size line, its CRLF, the chunk data, then the next size line. Erasing the
// consumed prefix on every step is quadratic in the body size, so this window
// advances an offset instead and drops the prefix only when that is both worth
// an erase (>= COMPACT_THRESHOLD bytes) and at least half of the buffer. The
// copying is then amortized O(1) per consumed byte.
//

#ifndef YADDNSC_INFRASTRUCTURE_NET_HTTP_PROTOCOL_READ_WINDOW_H
#define YADDNSC_INFRASTRUCTURE_NET_HTTP_PROTOCOL_READ_WINDOW_H

#include <cstddef>
#include <string>
#include <string_view>

namespace http::protocol {

/// A compacting sliding window over accumulated bytes.
///
/// Not thread-safe; owned by one decoding coroutine.
class ReadWindow {
public:
    /// Consumed-prefix size below which compaction is never attempted.
    static constexpr std::size_t COMPACT_THRESHOLD = 4096;

    /// Seed the window with bytes already read past the header block.
    explicit ReadWindow(std::string_view initial = {}) : data_(initial) {}

    /// The unconsumed bytes.
    [[nodiscard]] std::string_view view() const noexcept { return std::string_view(data_).substr(consumed_); }

    /// Unconsumed byte count.
    [[nodiscard]] std::size_t size() const noexcept { return data_.size() - consumed_; }

    /// Total bytes dropped from the front so far (tests/diagnostics).
    [[nodiscard]] std::size_t compacted_bytes() const noexcept { return compacted_; }

    /// Live buffer size, including the not-yet-dropped consumed prefix.
    [[nodiscard]] std::size_t capacity_in_use() const noexcept { return data_.size(); }

    /// Consume `n` bytes from the front. Precondition: `n <= size()`.
    void consume(const std::size_t n) noexcept {
        consumed_ += n;
        compact();
    }

    /// Append freshly read bytes.
    void append(const char* bytes, const std::size_t n) { data_.append(bytes, n); }

    /// Take the unconsumed remainder as a string, emptying the window.
    [[nodiscard]] std::string take_rest() {
        std::string rest{view()};
        data_.clear();
        consumed_ = 0;
        return rest;
    }

private:
    void compact() noexcept {
        if (consumed_ >= COMPACT_THRESHOLD && consumed_ * 2 >= data_.size()) {
            data_.erase(0, consumed_);
            compacted_ += consumed_;
            consumed_ = 0;
        }
    }

    std::string data_;
    std::size_t consumed_ = 0;
    std::size_t compacted_ = 0;
};

}  // namespace http::protocol

#endif  // YADDNSC_INFRASTRUCTURE_NET_HTTP_PROTOCOL_READ_WINDOW_H
