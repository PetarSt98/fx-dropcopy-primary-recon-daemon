#pragma once

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>

namespace persist {

// Collect wire log files from a directory using a filename prefix (lexicographic order).
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
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace persist
