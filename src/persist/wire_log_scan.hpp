#pragma once

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>
#include <charconv>

namespace persist {

struct WireLogFileInfo {
    std::filesystem::path path;
    std::uint64_t timestamp_key{0};
    std::uint64_t sequence{0};
    bool parsed{false};
};

inline bool parse_wire_log_filename(const std::filesystem::path& path,
                                    std::string_view prefix,
                                    WireLogFileInfo& out) {
    const auto filename = path.filename().string();
    if (!prefix.empty() && filename.rfind(prefix, 0) != 0) {
        return false;
    }
    // Expected: wire_capture_YYYYMMDD_HHMMSS_seqNNN.bin
    const auto stem = path.filename().stem().string();
    const std::string_view stem_view(stem);
    const auto prefix_len = prefix.size();
    if (stem_view.size() <= prefix_len + 15) {
        return false;
    }
    // Positions after prefix: YYYYMMDD_HHMMSS_seq...
    const std::string_view date_part = stem_view.substr(prefix_len, 8);
    const std::string_view time_part = stem_view.substr(prefix_len + 9, 6);
    const std::string_view seq_marker = "_seq";
    const auto seq_pos = stem_view.find(seq_marker, prefix_len + 15);
    if (seq_pos == std::string_view::npos) {
        return false;
    }
    const std::string_view seq_part = stem_view.substr(seq_pos + seq_marker.size());

    std::uint64_t yyyymmdd{0};
    auto res1 = std::from_chars(date_part.data(), date_part.data() + date_part.size(), yyyymmdd);
    if (res1.ec != std::errc() || res1.ptr != date_part.data() + date_part.size()) {
        return false;
    }
    std::uint64_t hhmmss{0};
    auto res2 = std::from_chars(time_part.data(), time_part.data() + time_part.size(), hhmmss);
    if (res2.ec != std::errc() || res2.ptr != time_part.data() + time_part.size()) {
        return false;
    }
    std::uint64_t seq{0};
    auto res3 = std::from_chars(seq_part.data(), seq_part.data() + seq_part.size(), seq);
    if (res3.ec != std::errc() || res3.ptr != seq_part.data() + seq_part.size()) {
        return false;
    }

    out.timestamp_key = yyyymmdd * 1000000ULL + hhmmss;
    out.sequence = seq;
    out.parsed = true;
    out.path = path;
    return true;
}

// Collect wire log files from a directory using a filename prefix (lexicographic order).
// This helper is intentionally small and non-throwing to keep replay startup deterministic.
inline std::vector<std::filesystem::path> scan_wire_logs(const std::filesystem::path& dir,
                                                         std::string_view prefix) {
    std::vector<WireLogFileInfo> infos;
    std::error_code ec;
    if (dir.empty()) {
        return {};
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        WireLogFileInfo info;
        if (!parse_wire_log_filename(entry.path(), prefix, info)) {
            info.path = entry.path();
        }
        infos.push_back(std::move(info));
    }
    std::sort(infos.begin(), infos.end(), [](const WireLogFileInfo& a, const WireLogFileInfo& b) {
        if (a.parsed != b.parsed) {
            return a.parsed; // parsed ones first
        }
        if (a.parsed && b.parsed) {
            if (a.timestamp_key == b.timestamp_key) {
                return a.sequence < b.sequence;
            }
            return a.timestamp_key < b.timestamp_key;
        }
        return a.path < b.path;
    });
    std::vector<std::filesystem::path> out;
    out.reserve(infos.size());
    for (auto& info : infos) {
        out.push_back(std::move(info.path));
    }
    return out;
}

} // namespace persist
