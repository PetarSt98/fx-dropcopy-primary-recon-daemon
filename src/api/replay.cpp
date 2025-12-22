#include "api/replay.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "api/replay_engine.hpp"
#include "persist/wire_log_format.hpp"
#include "persist/wire_log_scan.hpp"
#include "util/log.hpp"

namespace {

std::vector<std::filesystem::path> gather_input_files(const api::ReplayConfig& cfg) {
    if (!cfg.input_directory.empty()) {
        return persist::scan_wire_logs(cfg.input_directory, persist::default_filename_prefix());
    }
    return cfg.input_files;
}

std::string speed_to_string(const api::ReplayConfig& cfg) {
    if (cfg.fast) {
        return "fast";
    }
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss << cfg.speed;
    return oss.str();
}

struct CompareResult {
    bool match{false};
    std::string detail;
};

CompareResult compare_files_streaming(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa.is_open() || !fb.is_open()) {
        return {false, "failed to open files"};
    }
    fa.seekg(0, std::ios::end);
    fb.seekg(0, std::ios::end);
    const auto size_a = fa.tellg();
    const auto size_b = fb.tellg();
    if (size_a != size_b) {
        std::ostringstream oss;
        oss << "size mismatch: " << a << " (" << size_a << ") vs " << b << " (" << size_b << ")";
        return {false, oss.str()};
    }
    fa.seekg(0, std::ios::beg);
    fb.seekg(0, std::ios::beg);

    std::array<char, 4096> buf_a{};
    std::array<char, 4096> buf_b{};
    std::size_t offset = 0;
    while (fa && fb) {
        fa.read(buf_a.data(), static_cast<std::streamsize>(buf_a.size()));
        fb.read(buf_b.data(), static_cast<std::streamsize>(buf_b.size()));
        const auto read_a = static_cast<std::size_t>(fa.gcount());
        const auto read_b = static_cast<std::size_t>(fb.gcount());
        const auto chunk = std::min(read_a, read_b);
        for (std::size_t i = 0; i < chunk; ++i) {
            if (buf_a[i] != buf_b[i]) {
                std::ostringstream oss;
                oss << "mismatch at offset " << (offset + i)
                    << " lhs=" << static_cast<int>(static_cast<unsigned char>(buf_a[i]))
                    << " rhs=" << static_cast<int>(static_cast<unsigned char>(buf_b[i]));
                return {false, oss.str()};
            }
        }
        offset += chunk;
    }
    return {true, ""};
}

CompareResult compare_directories(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    if (lhs.empty() || rhs.empty()) {
        return {false, "empty directory path"};
    }
    std::error_code ec_l;
    std::error_code ec_r;
    if (!std::filesystem::exists(lhs, ec_l) || !std::filesystem::exists(rhs, ec_r)) {
        return {false, "one or both directories do not exist"};
    }
    std::vector<std::filesystem::path> left_files;
    std::vector<std::filesystem::path> right_files;
    auto collect = [](const std::filesystem::path& dir, std::vector<std::filesystem::path>& out) {
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file(ec) && !ec) {
                out.push_back(it->path());
            }
        }
        return ec.value() == 0;
    };
    if (!collect(lhs, left_files) || !collect(rhs, right_files)) {
        return {false, "failed to enumerate directories"};
    }
    auto normalize = [](const std::filesystem::path& p, const std::filesystem::path& root) {
        std::error_code ec;
        const auto rel = std::filesystem::relative(p, root, ec);
        return (ec ? p.filename() : rel).lexically_normal().string();
    };
    std::sort(left_files.begin(), left_files.end(), [&](const auto& a, const auto& b) {
        return normalize(a, lhs) < normalize(b, lhs);
    });
    std::sort(right_files.begin(), right_files.end(), [&](const auto& a, const auto& b) {
        return normalize(a, rhs) < normalize(b, rhs);
    });
    if (left_files.size() != right_files.size()) {
        std::ostringstream oss;
        oss << "file count mismatch: " << left_files.size() << " vs " << right_files.size();
        return {false, oss.str()};
    }
    for (std::size_t i = 0; i < left_files.size(); ++i) {
        const auto left_rel = normalize(left_files[i], lhs);
        const auto right_rel = normalize(right_files[i], rhs);
        if (left_rel != right_rel) {
            std::ostringstream oss;
            oss << "file ordering mismatch: " << left_rel << " vs " << right_rel;
            return {false, oss.str()};
        }
        auto cmp = compare_files_streaming(left_files[i], right_files[i]);
        if (!cmp.match) {
            if (cmp.detail.empty()) {
                cmp.detail = "files differ";
            }
            return cmp;
        }
    }
    return {true, ""};
}

} // namespace

namespace api {

int run_replay(const ReplayConfig& cfg) {
    replay_engine::ReplayConfig engine_cfg{
        .wire_inputs = gather_input_files(cfg),
        .from_ns = cfg.use_time_window ? std::optional<std::uint64_t>(cfg.window_start_ns) : std::nullopt,
        .to_ns = cfg.use_time_window ? std::optional<std::uint64_t>(cfg.window_end_ns) : std::nullopt,
        .config_path = {},
        .output_dir = cfg.output_dir.empty() ? std::filesystem::path("replay_out") : cfg.output_dir,
        .speed = speed_to_string(cfg),
        .max_records = cfg.max_records
    };

    std::string error_msg;
    const auto res = replay_engine::run_replay(engine_cfg, error_msg);
    if (res != replay_engine::ReplayResult::Success) {
        util::log(util::LogLevel::Error, "%s", error_msg.c_str());
        return 1;
    }

    if (!cfg.verify_against.empty()) {
        const auto cmp = compare_directories(engine_cfg.output_dir, cfg.verify_against);
        if (!cmp.match) {
            util::log(util::LogLevel::Error,
                      "Verification failed against %s: %s",
                      cfg.verify_against.string().c_str(),
                      cmp.detail.c_str());
            return 2;
        }
    }

    if (cfg.verbose && !cfg.quiet) {
        util::log(util::LogLevel::Info, "Replay completed");
    }

    return 0;
}

} // namespace api
