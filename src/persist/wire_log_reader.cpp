#include "persist/wire_log_reader.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

namespace persist {

namespace {

constexpr std::uint64_t max_u64 = std::numeric_limits<std::uint64_t>::max();

} // namespace

WireLogReader::WireLogReader(WireLogReaderOptions opts)
    : opts_(std::move(opts)) {
    if (opts_.window_end_ns == 0) {
        opts_.window_end_ns = max_u64;
    }
}

bool WireLogReader::prepare_files() {
    files_.clear();
    if (!opts_.files.empty()) {
        files_ = opts_.files;
    } else if (!opts_.directory.empty()) {
        files_ = scan_wire_logs(opts_.directory, opts_.filename_prefix);
    }
    std::sort(files_.begin(), files_.end());
    file_index_ = 0;
    return !files_.empty();
}

bool WireLogReader::open() {
    if (!prepare_files()) {
        return false;
    }
    return open_current_file();
}

bool WireLogReader::open_current_file() {
    close_file();
    while (file_index_ < files_.size()) {
        const auto& path = files_[file_index_];
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec != std::error_code{}) {
            ++stats_.io_errors;
            ++file_index_;
            continue;
        }
        if (size == 0) {
            ++file_index_;
            continue;
        }
        if (size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
            ++stats_.io_errors;
            ++file_index_;
            continue;
        }
        buffer_.resize(static_cast<std::size_t>(size));
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) {
            ++stats_.io_errors;
            ++file_index_;
            continue;
        }
        in.read(reinterpret_cast<char*>(buffer_.data()), static_cast<std::streamsize>(buffer_.size()));
        if (!in) {
            ++stats_.io_errors;
            ++file_index_;
            continue;
        }
        current_size_ = buffer_.size();
        offset_ = 0;
        have_current_ = true;
        ++stats_.files_opened;
        stats_.bytes_read += current_size_;
        return true;
    }
    return false;
}

void WireLogReader::close_file() noexcept {
    buffer_.clear();
    offset_ = 0;
    current_size_ = 0;
    have_current_ = false;
}

WireLogReadResult WireLogReader::handle_truncated() noexcept {
    ++stats_.truncated_tail;
    ++file_index_;
    close_file();
    return {WireLogReadStatus::Truncated};
}

bool WireLogReader::passes_time_window(std::uint64_t ts) const noexcept {
    if (!opts_.use_time_window) {
        return true;
    }
    if (ts < opts_.window_start_ns) {
        return false;
    }
    if (ts > opts_.window_end_ns) {
        return false;
    }
    return true;
}

WireLogReadResult WireLogReader::next(core::WireExecEvent& out, std::uint64_t& out_capture_ts) noexcept {
    WireLogReadStatus last_error = WireLogReadStatus::EndOfStream;
    while (true) {
        if (!have_current_) {
            if (file_index_ >= files_.size()) {
                return {last_error};
            }
            if (!open_current_file()) {
                return {WireLogReadStatus::IoError};
            }
        }
        if (offset_ == current_size_) {
            ++file_index_;
            close_file();
            continue;
        }
        if (offset_ + framed_size(0) > current_size_) {
            return handle_truncated();
        }
        const std::size_t remaining = current_size_ - offset_;
        const std::byte* ptr = buffer_.data() + offset_;

        WireRecordView view{};
        if (!parse_record(ptr, remaining, view)) {
            return handle_truncated();
        }
        const std::size_t total_size = framed_size(view.payload_length);
        offset_ += total_size;

        if (view.payload_length != wire_exec_event_serialized_size) {
            ++stats_.bad_length;
            ++stats_.records_corrupt;
            last_error = WireLogReadStatus::InvalidLength;
            continue;
        }
        if (!validate_record(view)) {
            ++stats_.checksum_failures;
            ++stats_.records_corrupt;
            last_error = WireLogReadStatus::ChecksumMismatch;
            continue;
        }

        if (!deserialize_wire_exec_event(out, view.payload.data(), view.payload_length)) {
            ++stats_.bad_length;
            ++stats_.records_corrupt;
            last_error = WireLogReadStatus::InvalidLength;
            continue;
        }
        out_capture_ts = out.sending_time;

        if (!passes_time_window(out_capture_ts)) {
            ++stats_.filtered_out;
            continue;
        }

        ++stats_.records_ok;
        return {WireLogReadStatus::Ok};
    }
}

} // namespace persist
