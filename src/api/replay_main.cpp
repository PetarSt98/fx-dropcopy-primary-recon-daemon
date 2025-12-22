#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "api/replay.hpp"

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --input <dir|file1,file2,...> [options]\n"
              << "Options:\n"
              << "  --from-ns <uint64>      Optional inclusive start timestamp (ns)\n"
              << "  --to-ns <uint64>        Optional inclusive end timestamp (ns)\n"
              << "  --speed <double>        Playback speed multiplier (default 1.0)\n"
              << "  --fast                  Process without sleeping (overrides --speed)\n"
              << "  --out-dir <path>        Output directory for audit logs (default ./replay_out)\n"
              << "  --verify-against <dir>  Compare outputs against golden directory\n"
              << "  --max-records <N>       Optional cap on records processed\n"
              << "  --quiet                 Suppress non-error logs\n"
              << "  --verbose               Enable verbose logging\n";
}

std::vector<std::filesystem::path> split_files(const std::string& s) {
    std::vector<std::filesystem::path> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            out.emplace_back(item);
        }
    }
    return out;
}

std::filesystem::path default_output_dir() {
    // Deterministic default to keep replay outputs stable across runs.
    return std::filesystem::path("replay_out");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    api::ReplayConfig cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            const std::string val = argv[++i];
            if (val.find(',') != std::string::npos) {
                cfg.input_files = split_files(val);
            } else {
                std::filesystem::path p(val);
                if (std::filesystem::is_regular_file(p)) {
                    cfg.input_files = {p};
                } else {
                    cfg.input_directory = p;
                }
            }
        } else if (arg == "--from-ns" && i + 1 < argc) {
            cfg.use_time_window = true;
            cfg.window_start_ns = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--to-ns" && i + 1 < argc) {
            cfg.use_time_window = true;
            cfg.window_end_ns = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--speed" && i + 1 < argc) {
            cfg.speed = std::strtod(argv[++i], nullptr);
        } else if (arg == "--fast") {
            cfg.fast = true;
        } else if (arg == "--out-dir" && i + 1 < argc) {
            cfg.output_dir = argv[++i];
        } else if (arg == "--verify-against" && i + 1 < argc) {
            cfg.verify_against = argv[++i];
        } else if (arg == "--max-records" && i + 1 < argc) {
            cfg.max_records = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--quiet") {
            cfg.quiet = true;
        } else if (arg == "--verbose") {
            cfg.verbose = true;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (cfg.output_dir.empty()) {
        cfg.output_dir = default_output_dir();
    }

    return run_replay(cfg);
}
