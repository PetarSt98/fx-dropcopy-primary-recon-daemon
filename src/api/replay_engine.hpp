#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace replay_engine {

struct ReplayConfig {
    std::vector<std::filesystem::path> wire_inputs;
    std::optional<std::uint64_t> from_ns;
    std::optional<std::uint64_t> to_ns;
    std::filesystem::path config_path;
    std::filesystem::path output_dir;
    std::string speed;              // "realtime", "fast", "max"
    std::size_t max_records;        // 0 = unlimited
};

enum class ReplayResult {
    Success,
    WireReadError,
    ConfigError,
    ReconError,
    OutputError
};

ReplayResult run_replay(const ReplayConfig& config, std::string& error_msg);

} // namespace replay_engine

