#pragma once

#include <bit>
#include <cstdint>

namespace persist {

inline constexpr std::uint16_t byteswap16(std::uint16_t v) noexcept {
    return static_cast<std::uint16_t>(((v & 0xFF00u) >> 8) | ((v & 0x00FFu) << 8));
}

inline constexpr std::uint32_t byteswap32(std::uint32_t v) noexcept {
    return ((v & 0xFF000000u) >> 24) |
           ((v & 0x00FF0000u) >> 8) |
           ((v & 0x0000FF00u) << 8) |
           ((v & 0x000000FFu) << 24);
}

inline constexpr std::uint64_t byteswap64(std::uint64_t v) noexcept {
    return ((v & 0x00000000000000FFull) << 56) |
           ((v & 0x000000000000FF00ull) << 40) |
           ((v & 0x0000000000FF0000ull) << 24) |
           ((v & 0x00000000FF000000ull) << 8) |
           ((v & 0x000000FF00000000ull) >> 8) |
           ((v & 0x0000FF0000000000ull) >> 24) |
           ((v & 0x00FF000000000000ull) >> 40) |
           ((v & 0xFF00000000000000ull) >> 56);
}

inline constexpr std::uint16_t to_le16(std::uint16_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return v;
    }
    return byteswap16(v);
}

inline constexpr std::uint16_t from_le16(std::uint16_t v) noexcept { return to_le16(v); }

inline constexpr std::uint32_t to_le32(std::uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return v;
    }
    return byteswap32(v);
}

inline constexpr std::uint32_t from_le32(std::uint32_t v) noexcept { return to_le32(v); }

inline constexpr std::uint64_t to_le64(std::uint64_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return v;
    }
    return byteswap64(v);
}

inline constexpr std::uint64_t from_le64(std::uint64_t v) noexcept { return to_le64(v); }

} // namespace persist

