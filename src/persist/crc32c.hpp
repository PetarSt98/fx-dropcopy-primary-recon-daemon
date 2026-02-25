#pragma once
// FX-8001: Software CRC-32C (Castagnoli, polynomial 0x1EDC6F41)
// Used for record integrity verification in the journal format.

#include <array>
#include <cstddef>
#include <cstdint>

namespace persist {

namespace detail {

constexpr std::uint32_t kCrc32cPoly = 0x82F63B78u; // bit-reversed Castagnoli

constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int j = 0; j < 8; ++j)
            c = (c >> 1) ^ ((c & 1) ? kCrc32cPoly : 0u);
        t[i] = c;
    }
    return t;
}

inline constexpr auto kCrc32cTable = make_crc32c_table();

} // namespace detail

/// Compute CRC-32C over [data, data+len).
/// To chain across chunks: pass the previous result as @p init.
inline std::uint32_t crc32c(const std::byte* data, std::size_t len,
                            std::uint32_t init = 0) noexcept {
    std::uint32_t crc = ~init;
    for (std::size_t i = 0; i < len; ++i)
        crc = detail::kCrc32cTable[(crc ^ static_cast<std::uint8_t>(data[i])) & 0xFFu]
              ^ (crc >> 8);
    return ~crc;
}

inline std::uint32_t crc32c(const void* data, std::size_t len,
                            std::uint32_t init = 0) noexcept {
    return crc32c(static_cast<const std::byte*>(data), len, init);
}

} // namespace persist
