#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "core/wire_exec_event.hpp"
#include "persist/wire_log_format.hpp"
#include "persist/wire_log_scan.hpp"

namespace persist {

// Wire log on-disk contract (matches WireCaptureWriter):
//   file header (24 bytes) followed by repeated records
//   record = [u32 payload_len_le][u64 capture_ts_ns_le][payload bytes][u32 crc32c_le]
// Payload is a serialized core::WireExecEvent (151 bytes).

struct WireLogReaderStats {
    std::uint64_t records_ok{0};
    std::uint64_t records_corrupt{0};
    std::uint64_t bytes_read{0};
    std::uint64_t files_opened{0};
    std::uint64_t truncated_tail{0};
    std::uint64_t checksum_failures{0};
    std::uint64_t bad_length{0};
    std::uint64_t io_errors{0};
    std::uint64_t filtered_out{0};
    std::uint64_t header_invalid{0};
};

struct WireLogReaderOptions {
    std::vector<std::filesystem::path> files{};
    std::filesystem::path directory{};
    std::string filename_prefix{std::string(default_filename_prefix())};
    std::uint64_t window_start_ns{0};
    std::uint64_t window_end_ns{std::numeric_limits<std::uint64_t>::max()};
    bool use_time_window{false};
};

enum class WireLogReadStatus {
    Ok = 0,
    EndOfStream,
    ChecksumMismatch,
    InvalidLength,
    Truncated,
    IoError,
};

struct WireLogReadResult {
    WireLogReadStatus status{WireLogReadStatus::EndOfStream};
};

class WireLogReader {
public:
    explicit WireLogReader(WireLogReaderOptions opts);

    // Collects files (explicit list or directory scan) and loads the first one.
    bool open();

    // Returns the next record. Never throws; advances past corrupt frames.
    WireLogReadResult next(core::WireExecEvent& out, std::uint64_t& out_capture_ts) noexcept;

    const WireLogReaderStats& stats() const noexcept { return stats_; }
    const std::vector<std::filesystem::path>& files() const noexcept { return files_; }

private:
    bool prepare_files();
    bool open_current_file();
    void close_file() noexcept;
    WireLogReadResult handle_truncated() noexcept;
    bool passes_time_window(std::uint64_t ts) const noexcept;

    WireLogReaderOptions opts_;
    WireLogReaderStats stats_;
    std::vector<std::filesystem::path> files_;
    std::vector<std::byte> buffer_;
    std::size_t offset_{0};
    std::size_t current_size_{0};
    std::size_t file_index_{0};
    bool have_current_{false};
};

} // namespace persist
