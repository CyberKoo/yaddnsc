//
// Created by Kotarou on 2026/7/8.
//

#ifndef YADDNSC_UTIL_BYTES_H
#define YADDNSC_UTIL_BYTES_H

#include <cstddef>
#include <cstdint>
#include <span>

/// Big-endian byte-order read helpers.
namespace Utils::Bytes
{

/// Read a 16-bit big-endian value from a raw pointer.
[[nodiscard]] constexpr std::uint16_t read_u16_be(const std::uint8_t* buf) noexcept
{
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(buf[0]) << 8) | static_cast<std::uint16_t>(buf[1]));
}

/// Read a 16-bit big-endian value from the start of a span.
[[nodiscard]] inline std::uint16_t read_u16_be(std::span<const std::uint8_t> buf) noexcept
{
  return read_u16_be(buf.data());
}

/// Read a 16-bit big-endian value from a span at the given offset.
[[nodiscard]] inline std::uint16_t read_u16_be(std::span<const std::uint8_t> buf, std::size_t offset) noexcept
{
  return read_u16_be(buf.subspan(offset));
}

/// Read a 32-bit big-endian value from a raw pointer.
[[nodiscard]] constexpr std::uint32_t read_u32_be(const std::uint8_t* buf) noexcept
{
  return (static_cast<std::uint32_t>(buf[0]) << 24) | (static_cast<std::uint32_t>(buf[1]) << 16) |
         (static_cast<std::uint32_t>(buf[2]) << 8) | static_cast<std::uint32_t>(buf[3]);
}

/// Read a 32-bit big-endian value from the start of a span.
[[nodiscard]] inline std::uint32_t read_u32_be(std::span<const std::uint8_t> buf) noexcept
{
  return read_u32_be(buf.data());
}

/// Read a 32-bit big-endian value from a span at the given offset.
[[nodiscard]] inline std::uint32_t read_u32_be(std::span<const std::uint8_t> buf, std::size_t offset) noexcept
{
  return read_u32_be(buf.data() + offset);
}

}  // namespace Utils::Bytes

#endif  // YADDNSC_UTIL_BYTES_H
