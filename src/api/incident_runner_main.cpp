#include <algorithm>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "api/incident_runner.hpp"
#include "api/replay_engine.hpp"
#include "persist/audit_diff.hpp"
#include "persist/incident_spec.hpp"
#include "util/crc32c.hpp"

namespace {

struct CliOptions {
    std::filesystem::path spec_path;
    bool refresh_golden{false};
    bool keep_temp{false};
    std::optional<std::filesystem::path> work_dir;
    std::optional<std::filesystem::path> baseline_config;
    std::optional<std::filesystem::path> candidate_config;
    std::optional<std::filesystem::path> whitelist;
    bool verbose{false};
    bool quiet{false};
};

struct ParsedReplayConfig {
    std::optional<std::size_t> max_records;
    std::optional<std::string> speed;
    std::string raw;
    std::string hash_hex;
};

struct RunPaths {
    std::filesystem::path work_root;
    std::filesystem::path golden_dir;
    std::filesystem::path golden_backup_dir;
    std::filesystem::path candidate_root;
    std::filesystem::path candidate_output_dir;
    std::filesystem::path candidate_diff_report;
    std::filesystem::path candidate_metadata;
};

constexpr int kExitSuccess = 0;
constexpr int kExitMismatch = 2;
constexpr int kExitSpecError = 3;
constexpr int kExitIoError = 4;
constexpr int kExitReplayError = 5;

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --spec <path> [options]\n"
              << "Options:\n"
              << "  --refresh-golden           Regenerate golden from baseline\n"
              << "  --keep-temp                Keep candidate output directories\n"
              << "  --work-dir <path>          Override work directory (default incident_runs/<id>/)\n"
              << "  --baseline-config <path>   Baseline config file\n"
              << "  --candidate-config <path>  Candidate config file\n"
              << "  --whitelist <path>         Whitelist JSON\n"
              << "  --verbose                  Verbose logging\n"
              << "  --quiet                    Quiet logging\n";
}

bool parse_cli(int argc, char** argv, CliOptions& out) {
    if (argc < 3) {
        print_usage(argv[0]);
        return false;
    }
    CliOptions opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--spec" && i + 1 < argc) {
            opts.spec_path = argv[++i];
        } else if (arg == "--refresh-golden") {
            opts.refresh_golden = true;
        } else if (arg == "--keep-temp") {
            opts.keep_temp = true;
        } else if (arg == "--work-dir" && i + 1 < argc) {
            opts.work_dir = std::filesystem::path(argv[++i]);
        } else if (arg == "--baseline-config" && i + 1 < argc) {
            opts.baseline_config = std::filesystem::path(argv[++i]);
        } else if (arg == "--candidate-config" && i + 1 < argc) {
            opts.candidate_config = std::filesystem::path(argv[++i]);
        } else if (arg == "--whitelist" && i + 1 < argc) {
            opts.whitelist = std::filesystem::path(argv[++i]);
        } else if (arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "--quiet") {
            opts.quiet = true;
        } else {
            print_usage(argv[0]);
            return false;
        }
    }
    out = std::move(opts);
    return true;
}

std::string format_error(int code, std::string_view description, std::string_view details, std::string_view action) {
    std::ostringstream oss;
    oss << "Exit " << code << ": " << description << "\n"
        << "Details: " << details << "\n"
        << "Action: " << action;
    return oss.str();
}

bool read_file(const std::filesystem::path& path, std::string& out, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        err = "Failed to open file: " + path.string();
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto len = in.tellg();
    if (len < 0) {
        err = "Failed to size file: " + path.string();
        return false;
    }
    out.resize(static_cast<std::size_t>(len));
    in.seekg(0, std::ios::beg);
    if (!in.read(out.data(), out.size())) {
        err = "Failed to read file: " + path.string();
        return false;
    }
    return true;
}

std::string crc32_hex(std::string_view data) {
    const auto crc = util::Crc32c::compute(reinterpret_cast<const std::byte*>(data.data()), data.size());
    std::ostringstream oss;
    oss << std::hex << std::uppercase;
    oss.width(8);
    oss.fill('0');
    oss << crc;
    return oss.str();
}

class JsonCursor {
public:
    explicit JsonCursor(std::string_view src) : src_(src) {}

    void skip_ws() const noexcept {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) {
            ++pos_;
        }
    }

    bool consume(char c) noexcept {
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool expect(char c) noexcept {
        skip_ws();
        if (pos_ >= src_.size() || src_[pos_] != c) {
            return false;
        }
        ++pos_;
        return true;
    }

    std::optional<std::string> parse_string(std::string& err) noexcept {
        skip_ws();
        if (pos_ >= src_.size() || src_[pos_] != '"') {
            err = "Expected string";
            return std::nullopt;
        }
        ++pos_;
        std::string out;
        while (pos_ < src_.size()) {
            const char c = src_[pos_++];
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                if (pos_ >= src_.size()) {
                    err = "Invalid escape";
                    return std::nullopt;
                }
                const char esc = src_[pos_++];
                switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default:
                    err = "Unsupported escape sequence";
                    return std::nullopt;
                }
            } else {
                out.push_back(c);
            }
        }
        err = "Unterminated string";
        return std::nullopt;
    }

    std::optional<std::uint64_t> parse_uint64(std::string& err) noexcept {
        skip_ws();
        const std::size_t start = pos_;
        while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            ++pos_;
        }
        if (start == pos_) {
            err = "Expected integer";
            return std::nullopt;
        }
        std::uint64_t value = 0;
        const auto* begin = src_.data() + start;
        const auto* end = src_.data() + pos_;
        const auto conv = std::from_chars(begin, end, value);
        if (conv.ec != std::errc()) {
            err = "Invalid integer";
            return std::nullopt;
        }
        return value;
    }

    bool skip_value(std::string& err) noexcept {
        skip_ws();
        if (pos_ >= src_.size()) {
            err = "Unexpected end of input";
            return false;
        }
        const char c = src_[pos_];
        if (c == '"') {
            auto str = parse_string(err);
            return str.has_value();
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '-') {
            // rudimentary number skip
            ++pos_;
            while (pos_ < src_.size() && (std::isdigit(static_cast<unsigned char>(src_[pos_])) || src_[pos_] == '.')) {
                ++pos_;
            }
            return true;
        }
        if (c == '{') {
            ++pos_;
            int depth = 1;
            while (pos_ < src_.size() && depth > 0) {
                if (src_[pos_] == '{') ++depth;
                else if (src_[pos_] == '}') --depth;
                ++pos_;
            }
            if (depth != 0) {
                err = "Unterminated object";
                return false;
            }
            return true;
        }
        if (c == '[') {
            ++pos_;
            int depth = 1;
            while (pos_ < src_.size() && depth > 0) {
                if (src_[pos_] == '[') ++depth;
                else if (src_[pos_] == ']') --depth;
                ++pos_;
            }
            if (depth != 0) {
                err = "Unterminated array";
                return false;
            }
            return true;
        }
        if (src_.substr(pos_).rfind("true", 0) == 0) { pos_ += 4; return true; }
        if (src_.substr(pos_).rfind("false", 0) == 0) { pos_ += 5; return true; }
        if (src_.substr(pos_).rfind("null", 0) == 0) { pos_ += 4; return true; }
        err = "Unexpected token";
        return false;
    }

private:
    mutable std::size_t pos_{0};
    std::string_view src_;
};

bool parse_replay_config_file(const std::filesystem::path& path, ParsedReplayConfig& out, std::string& err) {
    ParsedReplayConfig parsed{};
    if (!read_file(path, parsed.raw, err)) {
        return false;
    }
    parsed.hash_hex = crc32_hex(parsed.raw);

    JsonCursor cur(parsed.raw);
    if (!cur.expect('{')) {
        err = "Expected object";
        return false;
    }
    while (true) {
        cur.skip_ws();
        if (cur.consume('}')) {
            break;
        }
        std::string key_err;
        auto key = cur.parse_string(key_err);
        if (!key) { err = key_err; return false; }
        if (!cur.expect(':')) { err = "Expected ':'"; return false; }
        if (*key == "max_records") {
            auto v = cur.parse_uint64(key_err);
            if (!v) { err = key_err; return false; }
            parsed.max_records = static_cast<std::size_t>(*v);
        } else if (*key == "speed") {
            auto v = cur.parse_string(key_err);
            if (!v) { err = key_err; return false; }
            parsed.speed = *v;
        } else {
            if (!cur.skip_value(key_err)) { err = key_err; return false; }
        }
        cur.skip_ws();
        if (cur.consume('}')) {
            break;
        }
        if (!cur.consume(',')) { err = "Expected ','"; return false; }
    }
    out = std::move(parsed);
    return true;
}

std::string timestamp_label() {
    const auto now = std::chrono::system_clock::now();
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto value = secs.time_since_epoch().count();
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

RunPaths build_paths(const persist::IncidentSpec& spec, const CliOptions& cli) {
    RunPaths paths{};
    const auto base = cli.work_dir.value_or(std::filesystem::path("incident_runs") / spec.id);
    paths.work_root = base;
    paths.golden_dir = base / "golden";
    const std::string stamp = cli.work_dir ? std::string("fixed") : timestamp_label();
    const std::string run_dir = cli.work_dir ? std::string("run") : ("run_" + stamp);
    paths.candidate_root = base / run_dir;
    paths.candidate_output_dir = paths.candidate_root / "candidate";
    paths.candidate_diff_report = paths.candidate_root / "diff_report.txt";
    paths.candidate_metadata = paths.candidate_root / "metadata.json";
    paths.golden_backup_dir = cli.work_dir ? (base / "golden.backup") : (base / ("golden.backup." + stamp));
    return paths;
}

std::filesystem::path default_baseline_config(const persist::IncidentSpec& spec) {
    return std::filesystem::path("incidents") / spec.id / "baseline_config.json";
}

std::filesystem::path default_candidate_config(const persist::IncidentSpec& spec) {
    return std::filesystem::path("incidents") / spec.id / "candidate_config.json";
}

std::filesystem::path default_whitelist(const persist::IncidentSpec& spec) {
    return std::filesystem::path("incidents") / spec.id / "whitelist.json";
}

bool ensure_empty_dir(const std::filesystem::path& dir, std::string& err) {
    std::error_code ec;
    if (std::filesystem::exists(dir, ec)) {
        std::filesystem::remove_all(dir, ec);
        if (ec) {
            err = "Failed to clear directory: " + dir.string() + " (" + ec.message() + ")";
            return false;
        }
    }
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        err = "Failed to create directory: " + dir.string() + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

bool copy_dir(const std::filesystem::path& from, const std::filesystem::path& to, std::string& err) {
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    if (ec) {
        err = "Failed to create directory: " + to.string() + " (" + ec.message() + ")";
        return false;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(from, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) {
            continue;
        }
        const auto rel = std::filesystem::relative(it->path(), from, ec).lexically_normal();
        if (ec) {
            err = "Failed to compute relative path from " + it->path().string();
            return false;
        }
        const auto dest = to / rel;
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec) {
            err = "Failed to create directory: " + dest.parent_path().string();
            return false;
        }
        std::filesystem::copy_file(it->path(), dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            err = "Failed to copy file " + it->path().string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

bool write_metadata(const std::filesystem::path& path,
                    const persist::IncidentSpec& spec,
                    const std::filesystem::path& config_path,
                    const ParsedReplayConfig& cfg,
                    const replay_engine::ReplayConfig& replay_cfg,
                    std::string& err) {
    std::ostringstream oss;
    const auto now = std::chrono::system_clock::now();
    const auto ts = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    oss << "{\n"
        << "  \"incident_id\": \"" << spec.id << "\",\n"
        << "  \"config_path\": \"" << config_path.string() << "\",\n"
        << "  \"config_hash_crc32c\": \"" << cfg.hash_hex << "\",\n"
        << "  \"replay\": {\n"
        << "    \"speed\": \"" << replay_cfg.speed << "\",\n"
        << "    \"max_records\": " << replay_cfg.max_records << "\n"
        << "  },\n"
        << "  \"timestamp_ns\": " << ts << "\n"
        << "}\n";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        err = "Failed to write metadata: " + path.string();
        return false;
    }
    out << oss.str();
    if (!out) {
        err = "Failed to write metadata: " + path.string();
        return false;
    }
    return true;
}

std::filesystem::path resolve_relative(const std::filesystem::path& base_dir,
                                       const std::filesystem::path& p) {
    if (p.is_absolute()) {
        return p;
    }
    return (base_dir / p).lexically_normal();
}

std::vector<std::filesystem::path> resolve_wire_inputs(const persist::IncidentSpec& spec,
                                                       const std::filesystem::path& spec_dir) {
    std::vector<std::filesystem::path> out;
    for (const auto& wi : spec.wire_inputs) {
        out.push_back(resolve_relative(spec_dir, wi.path));
    }
    return out;
}

std::optional<replay_engine::ReplayConfig> build_replay_config(const persist::IncidentSpec& spec,
                                                               const ParsedReplayConfig& parsed_cfg,
                                                               const std::vector<std::filesystem::path>& inputs,
                                                               const std::filesystem::path& output_dir,
                                                               const std::filesystem::path& config_path,
                                                               std::string& err) {
    replay_engine::ReplayConfig cfg;
    cfg.wire_inputs = inputs;
    cfg.output_dir = output_dir;
    cfg.config_path = config_path;
    cfg.speed = parsed_cfg.speed.value_or(spec.replay.speed.empty() ? std::string("fast") : spec.replay.speed);
    cfg.max_records = parsed_cfg.max_records.value_or(spec.replay.max_records);
    if (!spec.wire_inputs.empty()) {
        std::uint64_t min_from = spec.wire_inputs.front().from_ns;
        std::uint64_t max_to = spec.wire_inputs.front().to_ns;
        for (const auto& wi : spec.wire_inputs) {
            min_from = std::min(min_from, wi.from_ns);
            max_to = std::max(max_to, wi.to_ns);
        }
        cfg.from_ns = min_from;
        cfg.to_ns = max_to;
    }
    if (cfg.max_records > std::numeric_limits<std::size_t>::max()) {
        err = "max_records exceeds size_t";
        return std::nullopt;
    }
    return cfg;
}

bool backup_golden(const std::filesystem::path& src, const std::filesystem::path& dst, std::string& err) {
    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        return true;
    }
    std::filesystem::remove_all(dst, ec);
    if (ec) {
        err = "Failed to clear golden backup: " + dst.string() + " (" + ec.message() + ")";
        return false;
    }
    return copy_dir(src, dst, err);
}

bool filter_directory_without_metadata(const std::filesystem::path& source,
                                       const std::filesystem::path& dest,
                                       std::string& err) {
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        err = "Failed to create directory: " + dest.string() + " (" + ec.message() + ")";
        return false;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(source, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) {
            continue;
        }
        if (it->path().filename() == "metadata.json") {
            continue;
        }
        const auto rel = std::filesystem::relative(it->path(), source, ec).lexically_normal();
        if (ec) {
            err = "Failed to compute relative path from " + it->path().string();
            return false;
        }
        const auto dest_path = dest / rel;
        std::filesystem::create_directories(dest_path.parent_path(), ec);
        if (ec) {
            err = "Failed to create directory: " + dest_path.parent_path().string();
            return false;
        }
        std::filesystem::copy_file(it->path(), dest_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            err = "Failed to copy file " + it->path().string() + ": " + ec.message();
            return false;
        }
    }
    return true;
}

int run_incident_runner(const CliOptions& cli) {
    if (cli.spec_path.empty()) {
        std::cerr << format_error(kExitSpecError,
                                  "Missing --spec",
                                  "No incident spec provided",
                                  "Invoke with --spec <path>") << std::endl;
        return kExitSpecError;
    }

    persist::IncidentSpec spec;
    std::string parse_err;
    if (!persist::parse_incident_spec(cli.spec_path, spec, parse_err)) {
        std::cerr << format_error(kExitSpecError,
                                  "Invalid incident spec",
                                  parse_err,
                                  "Fix the incident spec JSON and retry") << std::endl;
        return kExitSpecError;
    }

    const auto spec_dir = cli.spec_path.parent_path();
    const auto paths = build_paths(spec, cli);

    const auto baseline_cfg_path = cli.baseline_config.value_or(default_baseline_config(spec));
    const auto candidate_cfg_path = cli.candidate_config.value_or(default_candidate_config(spec));

    if (!std::filesystem::exists(baseline_cfg_path)) {
        std::cerr << format_error(kExitSpecError,
                                  "Config file not found",
                                  baseline_cfg_path.string() + " does not exist",
                                  "Create config file or use --baseline-config flag") << std::endl;
        return kExitSpecError;
    }
    if (!std::filesystem::exists(candidate_cfg_path)) {
        std::cerr << format_error(kExitSpecError,
                                  "Config file not found",
                                  candidate_cfg_path.string() + " does not exist",
                                  "Create config file or use --candidate-config flag") << std::endl;
        return kExitSpecError;
    }

    std::optional<std::filesystem::path> whitelist_path;
    if (cli.whitelist) {
        whitelist_path = *cli.whitelist;
    } else {
        const auto default_wl = default_whitelist(spec);
        if (std::filesystem::exists(default_wl)) {
            whitelist_path = default_wl;
        }
    }
    if (whitelist_path && !std::filesystem::exists(*whitelist_path)) {
        std::cerr << format_error(kExitSpecError,
                                  "Whitelist file not found",
                                  whitelist_path->string() + " does not exist",
                                  "Provide a valid whitelist path or omit --whitelist") << std::endl;
        return kExitSpecError;
    }

    ParsedReplayConfig baseline_cfg;
    if (!parse_replay_config_file(baseline_cfg_path, baseline_cfg, parse_err)) {
        std::cerr << format_error(kExitSpecError,
                                  "Invalid config file",
                                  parse_err,
                                  "Fix config JSON or provide a valid file") << std::endl;
        return kExitSpecError;
    }
    ParsedReplayConfig candidate_cfg;
    if (!parse_replay_config_file(candidate_cfg_path, candidate_cfg, parse_err)) {
        std::cerr << format_error(kExitSpecError,
                                  "Invalid config file",
                                  parse_err,
                                  "Fix config JSON or provide a valid file") << std::endl;
        return kExitSpecError;
    }

    std::vector<std::filesystem::path> wire_inputs = resolve_wire_inputs(spec, spec_dir);
    for (const auto& p : wire_inputs) {
        if (!std::filesystem::exists(p)) {
            std::cerr << format_error(kExitIoError,
                                      "Wire input missing",
                                      p.string() + " does not exist",
                                      "Provide the wire log file at the expected location") << std::endl;
            return kExitIoError;
        }
    }

    std::string err;
    std::error_code exists_ec;
    if (!std::filesystem::exists(paths.work_root, exists_ec)) {
        std::error_code ec;
        std::filesystem::create_directories(paths.work_root, ec);
        if (ec) {
            std::cerr << format_error(kExitIoError,
                                      "Cannot create work directory",
                                      ec.message(),
                                      "Check permissions or free space") << std::endl;
            return kExitIoError;
        }
    } else if (exists_ec) {
        std::cerr << format_error(kExitIoError,
                                  "Cannot stat work directory",
                                  exists_ec.message(),
                                  "Check permissions or free space") << std::endl;
        return kExitIoError;
    }

    const bool golden_exists = std::filesystem::exists(paths.golden_dir);
    if (!golden_exists && !cli.refresh_golden) {
        std::cerr << format_error(kExitSpecError,
                                  "Golden output missing",
                                  paths.golden_dir.string() + " does not exist",
                                  "Re-run with --refresh-golden to create golden outputs") << std::endl;
        return kExitSpecError;
    }

    if (cli.refresh_golden) {
        if (!backup_golden(paths.golden_dir, paths.golden_backup_dir, err)) {
            std::cerr << format_error(kExitIoError,
                                      "Failed to backup golden",
                                      err,
                                      "Ensure disk space and permissions, then retry") << std::endl;
            return kExitIoError;
        }
        if (!ensure_empty_dir(paths.golden_dir, err)) {
            std::cerr << format_error(kExitIoError,
                                      "Failed to prepare golden directory",
                                      err,
                                      "Check permissions and disk space") << std::endl;
            return kExitIoError;
        }
        auto replay_cfg = build_replay_config(spec, baseline_cfg, wire_inputs, paths.golden_dir, baseline_cfg_path, err);
        if (!replay_cfg) {
            std::cerr << format_error(kExitSpecError,
                                      "Invalid replay configuration",
                                      err,
                                      "Check speed and max_records values") << std::endl;
            return kExitSpecError;
        }
        std::string replay_err;
        auto res = replay_engine::run_replay(*replay_cfg, replay_err);
        if (res != replay_engine::ReplayResult::Success) {
            const int code = (res == replay_engine::ReplayResult::WireReadError) ? kExitIoError :
                             (res == replay_engine::ReplayResult::ConfigError ? kExitSpecError :
                             (res == replay_engine::ReplayResult::OutputError ? kExitIoError : kExitReplayError));
            std::cerr << format_error(code,
                                      "Baseline replay failed",
                                      replay_err,
                                      "Fix the baseline config or inputs and retry") << std::endl;
            return code;
        }
        if (!write_metadata(paths.golden_dir / "metadata.json", spec, baseline_cfg_path, baseline_cfg, *replay_cfg, err)) {
            std::cerr << format_error(kExitIoError,
                                      "Failed to write golden metadata",
                                      err,
                                      "Check permissions and disk space") << std::endl;
            return kExitIoError;
        }
    }

    if (!ensure_empty_dir(paths.candidate_root, err)) {
        std::cerr << format_error(kExitIoError,
                                  "Failed to prepare candidate directory",
                                  err,
                                  "Check permissions and disk space") << std::endl;
        return kExitIoError;
    }
    auto cand_cfg = build_replay_config(spec, candidate_cfg, wire_inputs, paths.candidate_output_dir, candidate_cfg_path, err);
    if (!cand_cfg) {
        std::cerr << format_error(kExitSpecError,
                                  "Invalid replay configuration",
                                  err,
                                  "Check speed and max_records values") << std::endl;
        return kExitSpecError;
    }
    std::string replay_err;
    auto cand_res = replay_engine::run_replay(*cand_cfg, replay_err);
    if (cand_res != replay_engine::ReplayResult::Success) {
        const int code = (cand_res == replay_engine::ReplayResult::WireReadError) ? kExitIoError :
                         (cand_res == replay_engine::ReplayResult::ConfigError ? kExitSpecError :
                         (cand_res == replay_engine::ReplayResult::OutputError ? kExitIoError : kExitReplayError));
        std::cerr << format_error(code,
                                  "Candidate replay failed",
                                  replay_err,
                                  "Fix the candidate config or inputs and retry") << std::endl;
        return code;
    }
    if (!write_metadata(paths.candidate_metadata, spec, candidate_cfg_path, candidate_cfg, *cand_cfg, err)) {
        std::cerr << format_error(kExitIoError,
                                  "Failed to write candidate metadata",
                                  err,
                                  "Check permissions and disk space") << std::endl;
        return kExitIoError;
    }

    std::filesystem::path expected = paths.golden_dir;
    std::filesystem::path actual = paths.candidate_output_dir;
    const auto compare_root = paths.candidate_root / "compare";
    const auto filtered_expected = compare_root / "expected";
    const auto filtered_actual = compare_root / "actual";
    if (!filter_directory_without_metadata(paths.golden_dir, filtered_expected, err) ||
        !filter_directory_without_metadata(paths.candidate_output_dir, filtered_actual, err)) {
        std::cerr << format_error(kExitIoError,
                                  "Failed to prepare comparison directories",
                                  err,
                                  "Check permissions and disk space") << std::endl;
        return kExitIoError;
    }
    expected = filtered_expected;
    actual = filtered_actual;

    persist::DiffOptions diff_opts;
    diff_opts.byte_for_byte = true;
    if (whitelist_path) {
        diff_opts.whitelist_path = *whitelist_path;
    } else {
        diff_opts.allow_whitelist = false;
    }
    persist::DiffStats stats{};
    std::string diff_report;
    auto diff_res = persist::diff_directories(expected, actual, diff_opts, stats, diff_report);
    if (!diff_report.empty()) {
        std::ofstream out(paths.candidate_diff_report, std::ios::binary | std::ios::trunc);
        if (out.is_open()) {
            out << diff_report;
        }
    }

    const bool refreshing = cli.refresh_golden;

    switch (diff_res) {
    case persist::DiffResult::Match:
        if (!cli.keep_temp && !cli.work_dir) {
            std::error_code ec;
            std::filesystem::remove_all(paths.candidate_root, ec);
        }
        return kExitSuccess;
    case persist::DiffResult::Mismatch:
        if (refreshing) {
            return kExitSuccess;
        }
        std::cerr << format_error(kExitMismatch,
                                  "Candidate differs from golden",
                                  "See diff_report.txt for details",
                                  "Review changes or update whitelist") << std::endl;
        return kExitMismatch;
    case persist::DiffResult::BadWhitelist:
        std::cerr << format_error(kExitSpecError,
                                  "Invalid whitelist",
                                  "Failed to parse whitelist file",
                                  "Fix whitelist JSON") << std::endl;
        return kExitSpecError;
    case persist::DiffResult::BadFormat:
        std::cerr << format_error(kExitSpecError,
                                  "Audit format error",
                                  "Unexpected audit log structure",
                                  "Regenerate golden outputs") << std::endl;
        return kExitSpecError;
    case persist::DiffResult::IoError:
        std::cerr << format_error(kExitIoError,
                                  "I/O error during comparison",
                                  "Failed to read output directories",
                                  "Check permissions and disk space") << std::endl;
        return kExitIoError;
    }

    return kExitReplayError;
}

} // namespace

namespace incident_runner {
int run_incident_runner_cli(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(const_cast<char*>("fx_incident_runner"));
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    CliOptions cli;
    if (!parse_cli(static_cast<int>(argv.size()), argv.data(), cli)) {
        return kExitSpecError;
    }
    return run_incident_runner(cli);
}

int run_incident_runner_main(int argc, char** argv) {
    CliOptions cli;
    if (!parse_cli(argc, argv, cli)) {
        return kExitSpecError;
    }
    return run_incident_runner(cli);
}
} // namespace incident_runner
