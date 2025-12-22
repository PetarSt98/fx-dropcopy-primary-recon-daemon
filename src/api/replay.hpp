#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace api {

struct ReplayConfig {
    std::vector<std::filesystem::path> input_files{};
    std::filesystem::path input_directory{};
    std::uint64_t window_start_ns{0};
    std::uint64_t window_end_ns{0};
    bool use_time_window{false};

    double speed{1.0};
    bool fast{false};

    std::filesystem::path output_dir{};
    std::filesystem::path verify_against{};
    std::size_t max_records{0};

    bool quiet{false};
    bool verbose{false};
};

// Runs the replay pipeline in-process. Returns 0 on success, non-zero on
// failure (including verification mismatches).
int run_replay(const ReplayConfig& cfg);

} // namespace api

