#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "persist/incident_spec.hpp"

namespace persist {

struct DiffOptions {
    bool byte_for_byte{true};
    bool allow_whitelist{true};
    std::filesystem::path whitelist_path;
};

struct DiffStats {
    std::size_t files_compared{0};
    std::size_t bytes_compared{0};
    std::size_t records_compared{0};
    std::size_t mismatches{0};
    std::size_t whitelisted{0};
};

enum class DiffResult {
    Match,
    Mismatch,
    IoError,
    BadFormat,
    BadWhitelist
};

DiffResult diff_directories(
    const std::filesystem::path& expected_dir,
    const std::filesystem::path& actual_dir,
    const DiffOptions& options,
    DiffStats& stats,
    std::string& out_report
) noexcept;

} // namespace persist
