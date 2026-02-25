#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace persist {

inline constexpr std::array<std::uint32_t, 256> crc32c_table = []() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int j = 0; j < 8; ++j) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : (crc >> 1);
        }
        t[i] = crc;
    }
    return t;
}();

/// Compute CRC-32C (Castagnoli) over a byte buffer.
/// Software lookup-table implementation — no external dependencies.
inline std::uint32_t crc32c(const std::byte* data, std::size_t len) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = crc32c_table[static_cast<std::uint8_t>(crc ^ static_cast<std::uint8_t>(data[i]))] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace persist
