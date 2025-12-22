#include "persist/audit_diff.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

#include "persist/audit_log_format.hpp"

namespace persist {
namespace {

constexpr std::size_t kChunkSize = 64 * 1024;
constexpr std::size_t kMaxRecordBytes = 1024 * 1024;
constexpr std::size_t kMaxReportMismatches = 10;

struct RecordData {
    DecodedRecord decoded;
    std::vector<std::byte> raw;
    std::uint64_t offset{0};
};

class RecordStreamParser {
public:
    bool feed(std::span<const std::byte> data) {
        buffer_.insert(buffer_.end(), data.begin(), data.end());
        if (buffer_.size() > kMaxRecordBytes) {
            return false;
        }
        return true;
    }

    enum class Status { Ok, NeedMore, Error, End };

    Status next_record(RecordData& out, std::string& err) {
        using enum Status;
        if (buffer_.empty()) {
            return NeedMore;
        }
        if (buffer_.size() < header_size) {
            return NeedMore;
        }
        const auto payload_len = load_le32(buffer_.data() + sizeof(std::uint32_t));
        const std::size_t total = header_size + payload_len + trailer_size;
        if (total > kMaxRecordBytes) {
            err = "Record exceeds max size";
            return Error;
        }
        if (buffer_.size() < total) {
            return NeedMore;
        }
        std::span<const std::byte> rec_span(buffer_.data(), total);
        DecodedRecord decoded{};
        const auto res = decode_record(rec_span, decoded);
        if (res != DecodeError::Ok) {
            if (is_graceful_eof(res) && total == header_size) {
                return End;
            }
            err = "Failed to decode record";
            return Error;
        }
        out.raw.assign(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total));
        out.decoded = decoded;
        out.offset = offset_;
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total));
        offset_ += total;
        return Ok;
    }

    bool empty() const noexcept { return buffer_.empty(); }

private:
    std::vector<std::byte> buffer_;
    std::uint64_t offset_{0};
};

struct MismatchDetail {
    std::filesystem::path file;
    std::uint64_t offset{0};
    std::size_t record_index{0};
    std::vector<std::byte> expected;
    std::vector<std::byte> actual;
    std::size_t first_diff{0};
    bool whitelisted{false};
    std::string whitelist_reason;
};

struct ExtraFileDetail {
    std::filesystem::path file;
    bool allowed{false};
};

std::string to_hex_bytes(std::span<const std::byte> data) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i > 0) {
            oss << ' ';
        }
        oss << "0x" << std::setw(2)
            << static_cast<unsigned int>(static_cast<unsigned char>(data[i]));
    }
    return oss.str();
}

bool list_files(const std::filesystem::path& root, std::vector<std::filesystem::path>& out) {
    if (!std::filesystem::exists(root)) {
        return false;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(root); it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file()) {
            out.push_back(std::filesystem::relative(it->path(), root).lexically_normal());
        }
    }
    auto comp = [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return a.lexically_normal().generic_string() < b.lexically_normal().generic_string();
    };
    std::sort(out.begin(), out.end(), comp);
    return true;
}

bool match_allow_patterns(const Whitelist& wl, const std::filesystem::path& file) {
    const auto fname = file.filename().generic_string();
    for (const auto& rule : wl.rules) {
        if (rule.type != WhitelistRuleType::AllowExtraFiles) {
            continue;
        }
        for (const auto& pat : rule.patterns) {
            if (wildcard_match(pat, fname)) {
                return true;
            }
        }
    }
    return false;
}

std::string describe_rule(const WhitelistRule& rule) {
    switch (rule.type) {
        case WhitelistRuleType::IgnoreDivergenceType:
            return "ignore_divergence_type";
        case WhitelistRuleType::IgnoreNOccurrences:
            return "ignore_n_occurrences";
        case WhitelistRuleType::IgnoreByOrderKey:
            return "ignore_by_order_key";
        case WhitelistRuleType::AllowExtraFiles:
            return "allow_extra_files";
    }
    return "unknown";
}

bool match_rule(const WhitelistRule& rule, const DecodedRecord& actual, std::size_t& remaining) {
    if (rule.type == WhitelistRuleType::AllowExtraFiles) {
        return false; // handled separately
    }
    if (actual.type != AuditRecordType::Divergence) {
        return false;
    }
    if (rule.type == WhitelistRuleType::IgnoreByOrderKey) {
        return std::find(rule.order_keys.begin(), rule.order_keys.end(), actual.divergence.key) != rule.order_keys.end();
    }
    if (rule.divergence_type != actual.divergence.type) {
        return false;
    }
    if (!rule.venue.empty()) {
        // Venue not encoded; ignore venue filter.
    }
    if (rule.type == WhitelistRuleType::IgnoreNOccurrences) {
        if (rule.remaining_occurrences == 0) {
            return false;
        }
        remaining = rule.remaining_occurrences - 1;
    }
    return true;
}

bool apply_whitelist(Whitelist& wl, const DecodedRecord& actual, std::string& reason) {
    for (auto& rule : wl.rules) {
        std::size_t remaining = rule.remaining_occurrences;
        if (match_rule(rule, actual, remaining)) {
            if (rule.type == WhitelistRuleType::IgnoreNOccurrences) {
                rule.remaining_occurrences = remaining;
            }
            reason = rule.reason.empty() ? describe_rule(rule) : rule.reason;
            return true;
        }
    }
    return false;
}

class ChunkedReader {
public:
    explicit ChunkedReader(const std::filesystem::path& path)
        : stream_(path, std::ios::binary), path_(path) {}

    bool ok() const { return stream_.good(); }

    enum class FetchResult { Record, End, Error };

    FetchResult fetch(RecordStreamParser& parser, RecordData& out, std::string& err) {
        while (true) {
            auto status = parser.next_record(out, err);
            if (status == RecordStreamParser::Status::Ok) {
                return FetchResult::Record;
            }
            if (status == RecordStreamParser::Status::Error) {
                return FetchResult::Error;
            }
            if (!stream_) {
                if (parser.empty()) {
                    err.clear();
                    return FetchResult::End;
                }
                err = "Truncated record";
                return FetchResult::Error;
            }
            std::array<std::byte, kChunkSize> buf{};
            stream_.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
            const auto got = stream_.gcount();
            if (got <= 0) {
                stream_.setstate(std::ios::eofbit);
                continue;
            }
            if (!parser.feed(std::span<const std::byte>(buf.data(), static_cast<std::size_t>(got)))) {
                err = "Record exceeds max size";
                return FetchResult::Error;
            }
        }
    }

    bool eof() const {
        return stream_.eof();
    }

private:
    std::ifstream stream_;
    std::filesystem::path path_;
};

std::size_t first_diff_offset(std::span<const std::byte> a, std::span<const std::byte> b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return n;
}

DiffResult diff_audit_records(const std::filesystem::path& expected_path,
                              const std::filesystem::path& actual_path,
                              Whitelist* wl,
                              const DiffOptions& options,
                              DiffStats& stats,
                              std::vector<MismatchDetail>& mismatches) {
    ChunkedReader expected_reader(expected_path);
    ChunkedReader actual_reader(actual_path);
    if (!expected_reader.ok() || !actual_reader.ok()) {
        return DiffResult::IoError;
    }
    RecordStreamParser expected_parser;
    RecordStreamParser actual_parser;
    RecordData expected_record;
    RecordData actual_record;
    std::size_t record_index = 0;
    std::string err_exp;
    std::string err_act;
    while (true) {
        auto have_exp = expected_reader.fetch(expected_parser, expected_record, err_exp);
        auto have_act = actual_reader.fetch(actual_parser, actual_record, err_act);
        if (have_exp == ChunkedReader::FetchResult::End && have_act == ChunkedReader::FetchResult::End) {
            break; // both exhausted
        }
        if (have_exp != ChunkedReader::FetchResult::Record || have_act != ChunkedReader::FetchResult::Record) {
            MismatchDetail detail;
            detail.file = expected_path.filename();
            detail.offset = (have_exp == ChunkedReader::FetchResult::Record) ? expected_record.offset : actual_record.offset;
            detail.record_index = record_index;
            if (have_exp == ChunkedReader::FetchResult::Error || have_act == ChunkedReader::FetchResult::Error) {
                return DiffResult::BadFormat;
            }
            mismatches.push_back(std::move(detail));
            ++stats.mismatches;
            break;
        }
        ++record_index;
        ++stats.records_compared;
        stats.bytes_compared += std::min(expected_record.raw.size(), actual_record.raw.size());
        if (expected_record.raw == actual_record.raw) {
            continue;
        }
        bool whitelisted = false;
        std::string reason;
        if (options.allow_whitelist && wl) {
            whitelisted = apply_whitelist(*wl, actual_record.decoded, reason);
        }
        MismatchDetail detail;
        detail.file = expected_path.filename();
        detail.offset = expected_record.offset;
        detail.record_index = record_index - 1;
        detail.first_diff = first_diff_offset(expected_record.raw, actual_record.raw);
        const std::size_t window = 16;
        const std::size_t start = detail.first_diff;
        const std::size_t exp_len = std::min(window, expected_record.raw.size() - start);
        const std::size_t act_len = std::min(window, actual_record.raw.size() - start);
        detail.expected.assign(expected_record.raw.begin() + static_cast<std::ptrdiff_t>(start),
                               expected_record.raw.begin() + static_cast<std::ptrdiff_t>(start + exp_len));
        detail.actual.assign(actual_record.raw.begin() + static_cast<std::ptrdiff_t>(start),
                             actual_record.raw.begin() + static_cast<std::ptrdiff_t>(start + act_len));
        detail.whitelisted = whitelisted;
        detail.whitelist_reason = reason;
        if (whitelisted) {
            ++stats.whitelisted;
        } else {
            ++stats.mismatches;
        }
        if (!whitelisted && mismatches.size() < kMaxReportMismatches) {
            mismatches.push_back(std::move(detail));
        } else if (whitelisted && mismatches.size() < kMaxReportMismatches) {
            mismatches.push_back(std::move(detail));
        }
    }
    if (!err_exp.empty() || !err_act.empty()) {
        return DiffResult::BadFormat;
    }
    return DiffResult::Match;
}

DiffResult diff_binary_files(const std::filesystem::path& expected_path,
                             const std::filesystem::path& actual_path,
                             DiffStats& stats,
                             std::vector<MismatchDetail>& mismatches) {
    std::ifstream exp(expected_path, std::ios::binary);
    std::ifstream act(actual_path, std::ios::binary);
    if (!exp || !act) {
        return DiffResult::IoError;
    }
    std::array<char, kChunkSize> buf_exp{};
    std::array<char, kChunkSize> buf_act{};
    std::uint64_t offset = 0;
    while (true) {
        exp.read(buf_exp.data(), static_cast<std::streamsize>(buf_exp.size()));
        act.read(buf_act.data(), static_cast<std::streamsize>(buf_act.size()));
        const auto got_e = exp.gcount();
        const auto got_a = act.gcount();
        if (got_e <= 0 && got_a <= 0) {
            break;
        }
        const std::size_t n = static_cast<std::size_t>(std::min(got_e, got_a));
        stats.bytes_compared += n;
        if (n > 0) {
            if (std::memcmp(buf_exp.data(), buf_act.data(), n) != 0) {
                MismatchDetail detail;
                detail.file = expected_path.filename();
                detail.offset = offset + first_diff_offset(
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf_exp.data()), n),
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(buf_act.data()), n));
                detail.record_index = 0;
                mismatches.push_back(std::move(detail));
                ++stats.mismatches;
                return DiffResult::Mismatch;
            }
        }
        if (got_e != got_a) {
            MismatchDetail detail;
            detail.file = expected_path.filename();
            detail.offset = offset + n;
            detail.record_index = 0;
            mismatches.push_back(std::move(detail));
            ++stats.mismatches;
            return DiffResult::Mismatch;
        }
        offset += n;
    }
    return DiffResult::Match;
}

std::string build_report(DiffResult result,
                         const DiffStats& stats,
                         const std::vector<MismatchDetail>& mismatches,
                         const std::vector<ExtraFileDetail>& extras) {
    std::ostringstream oss;
    oss << "--- Audit Diff Report ---\n";
    oss << "Status: " << (result == DiffResult::Match && stats.mismatches == 0 ? "Match" : "Mismatch") << "\n";
    oss << "Files compared: " << stats.files_compared << "\n";
    oss << "Records compared: " << stats.records_compared << "\n";
    oss << "Mismatches: " << stats.mismatches << "\n";
    oss << "Whitelisted: " << stats.whitelisted << "\n\n";
    if (!mismatches.empty()) {
        std::size_t idx = 1;
        for (const auto& m : mismatches) {
            oss << "Mismatch " << idx++ << ":\n";
            oss << "  File: " << m.file.generic_string() << "\n";
            oss << "  Offset: " << m.offset << " (0x" << std::hex << m.offset << std::dec << ")\n";
            oss << "  Record index: " << m.record_index << "\n";
            if (!m.expected.empty() || !m.actual.empty()) {
                oss << "  Expected: " << to_hex_bytes(m.expected) << "\n";
                oss << "  Actual:   " << to_hex_bytes(m.actual) << "\n";
                oss << "  First diff at byte " << m.first_diff << "\n";
            }
            if (m.whitelisted) {
                oss << "  Whitelist rule: " << m.whitelist_reason << "\n";
            }
            oss << "\n";
        }
    }
    if (!extras.empty()) {
        oss << "Extra files in actual:\n";
        for (const auto& e : extras) {
            oss << "  - " << e.file.generic_string() << " (" << (e.allowed ? "allowed" : "not allowed") << ")\n";
        }
    }
    oss << "--- End Report ---\n";
    return oss.str();
}

} // namespace

DiffResult diff_directories(
    const std::filesystem::path& expected_dir,
    const std::filesystem::path& actual_dir,
    const DiffOptions& options,
    DiffStats& stats,
    std::string& out_report
) noexcept {
    stats = {};
    Whitelist whitelist;
    Whitelist* wl_ptr = nullptr;
    if (options.allow_whitelist && !options.whitelist_path.empty()) {
        std::string err;
        if (!parse_whitelist(options.whitelist_path, whitelist, err)) {
            out_report = err;
            return DiffResult::BadWhitelist;
        }
        wl_ptr = &whitelist;
    }

    std::vector<std::filesystem::path> expected_files;
    std::vector<std::filesystem::path> actual_files;
    if (!list_files(expected_dir, expected_files) || !list_files(actual_dir, actual_files)) {
        out_report = "Failed to list directories";
        return DiffResult::IoError;
    }
    std::vector<ExtraFileDetail> extras;
    std::size_t exp_idx = 0;
    std::size_t act_idx = 0;
    std::vector<MismatchDetail> mismatches;
    auto normalize = [](const std::filesystem::path& p) {
        return p.lexically_normal().generic_string();
    };
    while (exp_idx < expected_files.size() && act_idx < actual_files.size()) {
        const auto& exp_rel = expected_files[exp_idx];
        const auto& act_rel = actual_files[act_idx];
        const auto exp_norm = normalize(exp_rel);
        const auto act_norm = normalize(act_rel);
        if (exp_norm == act_norm) {
            const auto exp_path = expected_dir / exp_rel;
            const auto act_path = actual_dir / act_rel;
            ++stats.files_compared;
            DiffResult res;
            if (options.byte_for_byte && (wl_ptr == nullptr || wl_ptr->empty())) {
                res = diff_binary_files(exp_path, act_path, stats, mismatches);
            } else {
                res = diff_audit_records(exp_path, act_path, wl_ptr, options, stats, mismatches);
            }
            if (res == DiffResult::IoError || res == DiffResult::BadFormat) {
                out_report = "Error diffing file: " + exp_rel.generic_string();
                return res;
            }
            ++exp_idx;
            ++act_idx;
            continue;
        }
        if (exp_norm < act_norm) {
            MismatchDetail detail;
            detail.file = exp_rel;
            mismatches.push_back(std::move(detail));
            ++stats.mismatches;
            ++exp_idx;
        } else {
            ExtraFileDetail extra;
            extra.file = act_rel;
            extra.allowed = wl_ptr && match_allow_patterns(*wl_ptr, act_rel);
            if (!extra.allowed) {
                ++stats.mismatches;
            } else {
                ++stats.whitelisted;
            }
            extras.push_back(extra);
            ++act_idx;
        }
    }
    while (exp_idx < expected_files.size()) {
        MismatchDetail detail;
        detail.file = expected_files[exp_idx];
        mismatches.push_back(std::move(detail));
        ++stats.mismatches;
        ++exp_idx;
    }
    while (act_idx < actual_files.size()) {
        ExtraFileDetail extra;
        extra.file = actual_files[act_idx];
        extra.allowed = wl_ptr && match_allow_patterns(*wl_ptr, actual_files[act_idx]);
        if (!extra.allowed) {
            ++stats.mismatches;
        } else {
            ++stats.whitelisted;
        }
        extras.push_back(extra);
        ++act_idx;
    }

    const DiffResult overall = stats.mismatches == 0 ? DiffResult::Match : DiffResult::Mismatch;
    out_report = build_report(overall, stats, mismatches, extras);
    return overall;
}

} // namespace persist
