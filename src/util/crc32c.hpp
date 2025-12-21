#pragma once

#include <cstdint>
#include <cstddef>

namespace util {

// Software CRC32C (Castagnoli) implementation, table-driven, deterministic across platforms.
// Optional hardware acceleration can hook into compute() if proven equivalent via tests.
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
    static constexpr std::uint32_t poly = 0x1EDC6F41u; // Castagnoli

    static constexpr std::uint32_t make_entry(std::uint32_t idx) noexcept {
        std::uint32_t c = idx;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (poly ^ (c >> 1)) : (c >> 1);
        }
        return c;
    }

    static constexpr std::uint32_t generate_entry(std::size_t i) noexcept { return make_entry(static_cast<std::uint32_t>(i)); }

    static constexpr std::uint32_t table_[256] = {
        generate_entry(0x00), generate_entry(0x01), generate_entry(0x02), generate_entry(0x03),
        generate_entry(0x04), generate_entry(0x05), generate_entry(0x06), generate_entry(0x07),
        generate_entry(0x08), generate_entry(0x09), generate_entry(0x0A), generate_entry(0x0B),
        generate_entry(0x0C), generate_entry(0x0D), generate_entry(0x0E), generate_entry(0x0F),
        generate_entry(0x10), generate_entry(0x11), generate_entry(0x12), generate_entry(0x13),
        generate_entry(0x14), generate_entry(0x15), generate_entry(0x16), generate_entry(0x17),
        generate_entry(0x18), generate_entry(0x19), generate_entry(0x1A), generate_entry(0x1B),
        generate_entry(0x1C), generate_entry(0x1D), generate_entry(0x1E), generate_entry(0x1F),
        generate_entry(0x20), generate_entry(0x21), generate_entry(0x22), generate_entry(0x23),
        generate_entry(0x24), generate_entry(0x25), generate_entry(0x26), generate_entry(0x27),
        generate_entry(0x28), generate_entry(0x29), generate_entry(0x2A), generate_entry(0x2B),
        generate_entry(0x2C), generate_entry(0x2D), generate_entry(0x2E), generate_entry(0x2F),
        generate_entry(0x30), generate_entry(0x31), generate_entry(0x32), generate_entry(0x33),
        generate_entry(0x34), generate_entry(0x35), generate_entry(0x36), generate_entry(0x37),
        generate_entry(0x38), generate_entry(0x39), generate_entry(0x3A), generate_entry(0x3B),
        generate_entry(0x3C), generate_entry(0x3D), generate_entry(0x3E), generate_entry(0x3F),
        generate_entry(0x40), generate_entry(0x41), generate_entry(0x42), generate_entry(0x43),
        generate_entry(0x44), generate_entry(0x45), generate_entry(0x46), generate_entry(0x47),
        generate_entry(0x48), generate_entry(0x49), generate_entry(0x4A), generate_entry(0x4B),
        generate_entry(0x4C), generate_entry(0x4D), generate_entry(0x4E), generate_entry(0x4F),
        generate_entry(0x50), generate_entry(0x51), generate_entry(0x52), generate_entry(0x53),
        generate_entry(0x54), generate_entry(0x55), generate_entry(0x56), generate_entry(0x57),
        generate_entry(0x58), generate_entry(0x59), generate_entry(0x5A), generate_entry(0x5B),
        generate_entry(0x5C), generate_entry(0x5D), generate_entry(0x5E), generate_entry(0x5F),
        generate_entry(0x60), generate_entry(0x61), generate_entry(0x62), generate_entry(0x63),
        generate_entry(0x64), generate_entry(0x65), generate_entry(0x66), generate_entry(0x67),
        generate_entry(0x68), generate_entry(0x69), generate_entry(0x6A), generate_entry(0x6B),
        generate_entry(0x6C), generate_entry(0x6D), generate_entry(0x6E), generate_entry(0x6F),
        generate_entry(0x70), generate_entry(0x71), generate_entry(0x72), generate_entry(0x73),
        generate_entry(0x74), generate_entry(0x75), generate_entry(0x76), generate_entry(0x77),
        generate_entry(0x78), generate_entry(0x79), generate_entry(0x7A), generate_entry(0x7B),
        generate_entry(0x7C), generate_entry(0x7D), generate_entry(0x7E), generate_entry(0x7F),
        generate_entry(0x80), generate_entry(0x81), generate_entry(0x82), generate_entry(0x83),
        generate_entry(0x84), generate_entry(0x85), generate_entry(0x86), generate_entry(0x87),
        generate_entry(0x88), generate_entry(0x89), generate_entry(0x8A), generate_entry(0x8B),
        generate_entry(0x8C), generate_entry(0x8D), generate_entry(0x8E), generate_entry(0x8F),
        generate_entry(0x90), generate_entry(0x91), generate_entry(0x92), generate_entry(0x93),
        generate_entry(0x94), generate_entry(0x95), generate_entry(0x96), generate_entry(0x97),
        generate_entry(0x98), generate_entry(0x99), generate_entry(0x9A), generate_entry(0x9B),
        generate_entry(0x9C), generate_entry(0x9D), generate_entry(0x9E), generate_entry(0x9F),
        generate_entry(0xA0), generate_entry(0xA1), generate_entry(0xA2), generate_entry(0xA3),
        generate_entry(0xA4), generate_entry(0xA5), generate_entry(0xA6), generate_entry(0xA7),
        generate_entry(0xA8), generate_entry(0xA9), generate_entry(0xAA), generate_entry(0xAB),
        generate_entry(0xAC), generate_entry(0xAD), generate_entry(0xAE), generate_entry(0xAF),
        generate_entry(0xB0), generate_entry(0xB1), generate_entry(0xB2), generate_entry(0xB3),
        generate_entry(0xB4), generate_entry(0xB5), generate_entry(0xB6), generate_entry(0xB7),
        generate_entry(0xB8), generate_entry(0xB9), generate_entry(0xBA), generate_entry(0xBB),
        generate_entry(0xBC), generate_entry(0xBD), generate_entry(0xBE), generate_entry(0xBF),
        generate_entry(0xC0), generate_entry(0xC1), generate_entry(0xC2), generate_entry(0xC3),
        generate_entry(0xC4), generate_entry(0xC5), generate_entry(0xC6), generate_entry(0xC7),
        generate_entry(0xC8), generate_entry(0xC9), generate_entry(0xCA), generate_entry(0xCB),
        generate_entry(0xCC), generate_entry(0xCD), generate_entry(0xCE), generate_entry(0xCF),
        generate_entry(0xD0), generate_entry(0xD1), generate_entry(0xD2), generate_entry(0xD3),
        generate_entry(0xD4), generate_entry(0xD5), generate_entry(0xD6), generate_entry(0xD7),
        generate_entry(0xD8), generate_entry(0xD9), generate_entry(0xDA), generate_entry(0xDB),
        generate_entry(0xDC), generate_entry(0xDD), generate_entry(0xDE), generate_entry(0xDF),
        generate_entry(0xE0), generate_entry(0xE1), generate_entry(0xE2), generate_entry(0xE3),
        generate_entry(0xE4), generate_entry(0xE5), generate_entry(0xE6), generate_entry(0xE7),
        generate_entry(0xE8), generate_entry(0xE9), generate_entry(0xEA), generate_entry(0xEB),
        generate_entry(0xEC), generate_entry(0xED), generate_entry(0xEE), generate_entry(0xEF),
        generate_entry(0xF0), generate_entry(0xF1), generate_entry(0xF2), generate_entry(0xF3),
        generate_entry(0xF4), generate_entry(0xF5), generate_entry(0xF6), generate_entry(0xF7),
        generate_entry(0xF8), generate_entry(0xF9), generate_entry(0xFA), generate_entry(0xFB),
        generate_entry(0xFC), generate_entry(0xFD), generate_entry(0xFE), generate_entry(0xFF),
    };
};

} // namespace util
