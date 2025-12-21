#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <string_view>
#include <bit>

#include "util/crc32c.hpp"
#include "persist/endianness.hpp"

namespace persist {

struct WireRecordView {
    std::uint32_t payload_length{0};
    std::span<const std::byte> payload{};
    std::uint32_t checksum{0};
};

inline constexpr std::size_t framed_size(std::size_t payload_len) noexcept {
    return sizeof(std::uint32_t) + payload_len + sizeof(std::uint32_t);
}

inline std::uint32_t crc32c(std::span<const std::byte> payload) noexcept {
    return util::Crc32c::compute(payload.data(), payload.size());
}

inline void encode_record(std::span<const std::byte> payload,
                          std::uint32_t checksum,
                          std::uint32_t& length_le,
                          std::uint32_t& checksum_le) noexcept {
    length_le = to_le32(static_cast<std::uint32_t>(payload.size()));
    checksum_le = to_le32(checksum);
}

inline bool parse_record(const std::byte* data,
                         std::size_t size,
                         WireRecordView& out) noexcept {
    if (size < framed_size(0)) {
        return false;
    }
    const auto len_le = *reinterpret_cast<const std::uint32_t*>(data);
    const std::uint32_t payload_len = from_le32(len_le);
    const std::size_t total = framed_size(payload_len);
    if (total > size) {
        return false;
    }
    out.payload_length = payload_len;
    out.payload = std::span<const std::byte>(data + sizeof(std::uint32_t), payload_len);
    out.checksum = from_le32(*reinterpret_cast<const std::uint32_t*>(data + sizeof(std::uint32_t) + payload_len));
    return true;
}

inline bool validate_record(const WireRecordView& rec) noexcept {
    return crc32c(rec.payload) == rec.checksum;
}

inline std::string_view default_filename_prefix() noexcept {
    return "wire_capture_";
}

} // namespace persist
