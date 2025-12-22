#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>
#include <tuple>

namespace persist {

struct ParsedWireLogName {
    int year{0};
    int month{0};
    int day{0};
    int hour{0};
    int minute{0};
    int second{0};
    int sequence{0};
    bool valid{false};
};

inline bool parse_wire_log_name(const std::string& filename,
                                std::string_view prefix,
                                ParsedWireLogName& out) noexcept {
    if (filename.size() < prefix.size() + 8 + 1 + 6 + 4) {
        return false;
    }
    if (!prefix.empty() && filename.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::size_t date_start = prefix.size();
    if (filename.size() < date_start + 8 + 1 + 6 + 4) {
        return false;
    }
    auto to_int = [](std::string_view s, int& out_val) noexcept {
        int v = 0;
        for (char c : s) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return false;
            }
            v = v * 10 + (c - '0');
        }
        out_val = v;
        return true;
    };
    const std::string_view date = std::string_view(filename).substr(date_start, 8);
    const std::size_t time_start = date_start + 9; // skip '_'
    if (filename[date_start + 8] != '_' || filename.size() < time_start + 6 + 4) {
        return false;
    }
    const std::string_view time = std::string_view(filename).substr(time_start, 6);
    const std::size_t seq_label_start = time_start + 6;
    if (filename.substr(seq_label_start, 4) != "_seq") {
        return false;
    }
    const std::size_t seq_start = seq_label_start + 4;
    const std::size_t dot = filename.rfind('.');
    if (dot == std::string::npos || dot <= seq_start) {
        return false;
    }
    const std::string_view seq = std::string_view(filename).substr(seq_start, dot - seq_start);

    ParsedWireLogName parsed{};
    if (!to_int(date.substr(0, 4), parsed.year) ||
        !to_int(date.substr(4, 2), parsed.month) ||
        !to_int(date.substr(6, 2), parsed.day) ||
        !to_int(time.substr(0, 2), parsed.hour) ||
        !to_int(time.substr(2, 2), parsed.minute) ||
        !to_int(time.substr(4, 2), parsed.second) ||
        !to_int(seq, parsed.sequence)) {
        return false;
    }
    parsed.valid = true;
    out = parsed;
    return true;
}

inline void sort_wire_logs(std::vector<std::filesystem::path>& files, std::string_view prefix) {
    std::sort(files.begin(), files.end(), [prefix](const auto& a, const auto& b) {
        ParsedWireLogName pa{}, pb{};
        const bool ok_a = parse_wire_log_name(a.filename().string(), prefix, pa);
        const bool ok_b = parse_wire_log_name(b.filename().string(), prefix, pb);
        if (ok_a && ok_b) {
            return std::tie(pa.year, pa.month, pa.day, pa.hour, pa.minute, pa.second, pa.sequence) <
                   std::tie(pb.year, pb.month, pb.day, pb.hour, pb.minute, pb.second, pb.sequence);
        }
        if (ok_a != ok_b) {
            return ok_a; // parsed names come first
        }
        return a.filename() < b.filename();
    });
}

// Collect wire log files from a directory using a filename prefix, ordered by timestamp+sequence.
// This helper is intentionally small and non-throwing to keep replay startup deterministic.
inline std::vector<std::filesystem::path> scan_wire_logs(const std::filesystem::path& dir,
                                                         std::string_view prefix) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (dir.empty()) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto filename = entry.path().filename().string();
        if (!prefix.empty() && filename.rfind(prefix, 0) != 0) {
            continue;
        }
        out.push_back(entry.path());
    }
    sort_wire_logs(out, prefix);
    return out;
}

} // namespace persist
