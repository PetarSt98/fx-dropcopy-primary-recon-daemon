#pragma once

#include <cstdint>
#include <cstddef>

namespace util {

// Software CRC32C (Castagnoli) implementation, table-driven, deterministic across platforms.
// Optional hardware acceleration can hook into compute() if proven equivalent via tests.
namespace detail {

// Reflected Castagnoli polynomial for CRC32C
constexpr std::uint32_t poly = 0x82F63B78u;

constexpr std::uint32_t make_entry(std::uint32_t idx) noexcept {
    std::uint32_t c = idx;
    for (int k = 0; k < 8; ++k) {
        c = (c & 1u) ? (poly ^ (c >> 1)) : (c >> 1);
    }
    return c;
}

constexpr std::uint32_t generate_entry(std::size_t i) noexcept {
    return make_entry(static_cast<std::uint32_t>(i));
}

} // namespace detail

class Crc32c {
public:
    static constexpr std::uint32_t initial = 0xFFFFFFFFu;
    static constexpr std::uint32_t xor_out = 0xFFFFFFFFu;

    static std::uint32_t update(std::uint32_t crc, const std::byte* data, std::size_t len) noexcept {
        std::uint32_t c = crc;
        for (std::size_t i = 0; i < len; ++i) {
            const std::uint8_t idx = static_cast<std::uint8_t>((c ^ static_cast<std::uint8_t>(data[i])) & 0xFFu);
            c = (c >> 8) ^ table_[idx];
        }
        return c;
    }

    static std::uint32_t compute(const std::byte* data, std::size_t len) noexcept {
        return finalize(update(initial, data, len));
    }

    static constexpr std::uint32_t finalize(std::uint32_t crc) noexcept { return crc ^ xor_out; }

private:
    static constexpr std::uint32_t table_[256] = {
        detail::generate_entry(0x00), detail::generate_entry(0x01), detail::generate_entry(0x02), detail::generate_entry(0x03),
        detail::generate_entry(0x04), detail::generate_entry(0x05), detail::generate_entry(0x06), detail::generate_entry(0x07),
        detail::generate_entry(0x08), detail::generate_entry(0x09), detail::generate_entry(0x0A), detail::generate_entry(0x0B),
        detail::generate_entry(0x0C), detail::generate_entry(0x0D), detail::generate_entry(0x0E), detail::generate_entry(0x0F),
        detail::generate_entry(0x10), detail::generate_entry(0x11), detail::generate_entry(0x12), detail::generate_entry(0x13),
        detail::generate_entry(0x14), detail::generate_entry(0x15), detail::generate_entry(0x16), detail::generate_entry(0x17),
        detail::generate_entry(0x18), detail::generate_entry(0x19), detail::generate_entry(0x1A), detail::generate_entry(0x1B),
        detail::generate_entry(0x1C), detail::generate_entry(0x1D), detail::generate_entry(0x1E), detail::generate_entry(0x1F),
        detail::generate_entry(0x20), detail::generate_entry(0x21), detail::generate_entry(0x22), detail::generate_entry(0x23),
        detail::generate_entry(0x24), detail::generate_entry(0x25), detail::generate_entry(0x26), detail::generate_entry(0x27),
        detail::generate_entry(0x28), detail::generate_entry(0x29), detail::generate_entry(0x2A), detail::generate_entry(0x2B),
        detail::generate_entry(0x2C), detail::generate_entry(0x2D), detail::generate_entry(0x2E), detail::generate_entry(0x2F),
        detail::generate_entry(0x30), detail::generate_entry(0x31), detail::generate_entry(0x32), detail::generate_entry(0x33),
        detail::generate_entry(0x34), detail::generate_entry(0x35), detail::generate_entry(0x36), detail::generate_entry(0x37),
        detail::generate_entry(0x38), detail::generate_entry(0x39), detail::generate_entry(0x3A), detail::generate_entry(0x3B),
        detail::generate_entry(0x3C), detail::generate_entry(0x3D), detail::generate_entry(0x3E), detail::generate_entry(0x3F),
        detail::generate_entry(0x40), detail::generate_entry(0x41), detail::generate_entry(0x42), detail::generate_entry(0x43),
        detail::generate_entry(0x44), detail::generate_entry(0x45), detail::generate_entry(0x46), detail::generate_entry(0x47),
        detail::generate_entry(0x48), detail::generate_entry(0x49), detail::generate_entry(0x4A), detail::generate_entry(0x4B),
        detail::generate_entry(0x4C), detail::generate_entry(0x4D), detail::generate_entry(0x4E), detail::generate_entry(0x4F),
        detail::generate_entry(0x50), detail::generate_entry(0x51), detail::generate_entry(0x52), detail::generate_entry(0x53),
        detail::generate_entry(0x54), detail::generate_entry(0x55), detail::generate_entry(0x56), detail::generate_entry(0x57),
        detail::generate_entry(0x58), detail::generate_entry(0x59), detail::generate_entry(0x5A), detail::generate_entry(0x5B),
        detail::generate_entry(0x5C), detail::generate_entry(0x5D), detail::generate_entry(0x5E), detail::generate_entry(0x5F),
        detail::generate_entry(0x60), detail::generate_entry(0x61), detail::generate_entry(0x62), detail::generate_entry(0x63),
        detail::generate_entry(0x64), detail::generate_entry(0x65), detail::generate_entry(0x66), detail::generate_entry(0x67),
        detail::generate_entry(0x68), detail::generate_entry(0x69), detail::generate_entry(0x6A), detail::generate_entry(0x6B),
        detail::generate_entry(0x6C), detail::generate_entry(0x6D), detail::generate_entry(0x6E), detail::generate_entry(0x6F),
        detail::generate_entry(0x70), detail::generate_entry(0x71), detail::generate_entry(0x72), detail::generate_entry(0x73),
        detail::generate_entry(0x74), detail::generate_entry(0x75), detail::generate_entry(0x76), detail::generate_entry(0x77),
        detail::generate_entry(0x78), detail::generate_entry(0x79), detail::generate_entry(0x7A), detail::generate_entry(0x7B),
        detail::generate_entry(0x7C), detail::generate_entry(0x7D), detail::generate_entry(0x7E), detail::generate_entry(0x7F),
        detail::generate_entry(0x80), detail::generate_entry(0x81), detail::generate_entry(0x82), detail::generate_entry(0x83),
        detail::generate_entry(0x84), detail::generate_entry(0x85), detail::generate_entry(0x86), detail::generate_entry(0x87),
        detail::generate_entry(0x88), detail::generate_entry(0x89), detail::generate_entry(0x8A), detail::generate_entry(0x8B),
        detail::generate_entry(0x8C), detail::generate_entry(0x8D), detail::generate_entry(0x8E), detail::generate_entry(0x8F),
        detail::generate_entry(0x90), detail::generate_entry(0x91), detail::generate_entry(0x92), detail::generate_entry(0x93),
        detail::generate_entry(0x94), detail::generate_entry(0x95), detail::generate_entry(0x96), detail::generate_entry(0x97),
        detail::generate_entry(0x98), detail::generate_entry(0x99), detail::generate_entry(0x9A), detail::generate_entry(0x9B),
        detail::generate_entry(0x9C), detail::generate_entry(0x9D), detail::generate_entry(0x9E), detail::generate_entry(0x9F),
        detail::generate_entry(0xA0), detail::generate_entry(0xA1), detail::generate_entry(0xA2), detail::generate_entry(0xA3),
        detail::generate_entry(0xA4), detail::generate_entry(0xA5), detail::generate_entry(0xA6), detail::generate_entry(0xA7),
        detail::generate_entry(0xA8), detail::generate_entry(0xA9), detail::generate_entry(0xAA), detail::generate_entry(0xAB),
        detail::generate_entry(0xAC), detail::generate_entry(0xAD), detail::generate_entry(0xAE), detail::generate_entry(0xAF),
        detail::generate_entry(0xB0), detail::generate_entry(0xB1), detail::generate_entry(0xB2), detail::generate_entry(0xB3),
        detail::generate_entry(0xB4), detail::generate_entry(0xB5), detail::generate_entry(0xB6), detail::generate_entry(0xB7),
        detail::generate_entry(0xB8), detail::generate_entry(0xB9), detail::generate_entry(0xBA), detail::generate_entry(0xBB),
        detail::generate_entry(0xBC), detail::generate_entry(0xBD), detail::generate_entry(0xBE), detail::generate_entry(0xBF),
        detail::generate_entry(0xC0), detail::generate_entry(0xC1), detail::generate_entry(0xC2), detail::generate_entry(0xC3),
        detail::generate_entry(0xC4), detail::generate_entry(0xC5), detail::generate_entry(0xC6), detail::generate_entry(0xC7),
        detail::generate_entry(0xC8), detail::generate_entry(0xC9), detail::generate_entry(0xCA), detail::generate_entry(0xCB),
        detail::generate_entry(0xCC), detail::generate_entry(0xCD), detail::generate_entry(0xCE), detail::generate_entry(0xCF),
        detail::generate_entry(0xD0), detail::generate_entry(0xD1), detail::generate_entry(0xD2), detail::generate_entry(0xD3),
        detail::generate_entry(0xD4), detail::generate_entry(0xD5), detail::generate_entry(0xD6), detail::generate_entry(0xD7),
        detail::generate_entry(0xD8), detail::generate_entry(0xD9), detail::generate_entry(0xDA), detail::generate_entry(0xDB),
        detail::generate_entry(0xDC), detail::generate_entry(0xDD), detail::generate_entry(0xDE), detail::generate_entry(0xDF),
        detail::generate_entry(0xE0), detail::generate_entry(0xE1), detail::generate_entry(0xE2), detail::generate_entry(0xE3),
        detail::generate_entry(0xE4), detail::generate_entry(0xE5), detail::generate_entry(0xE6), detail::generate_entry(0xE7),
        detail::generate_entry(0xE8), detail::generate_entry(0xE9), detail::generate_entry(0xEA), detail::generate_entry(0xEB),
        detail::generate_entry(0xEC), detail::generate_entry(0xED), detail::generate_entry(0xEE), detail::generate_entry(0xEF),
        detail::generate_entry(0xF0), detail::generate_entry(0xF1), detail::generate_entry(0xF2), detail::generate_entry(0xF3),
        detail::generate_entry(0xF4), detail::generate_entry(0xF5), detail::generate_entry(0xF6), detail::generate_entry(0xF7),
        detail::generate_entry(0xF8), detail::generate_entry(0xF9), detail::generate_entry(0xFA), detail::generate_entry(0xFB),
        detail::generate_entry(0xFC), detail::generate_entry(0xFD), detail::generate_entry(0xFE), detail::generate_entry(0xFF),
    };
};

} // namespace util
