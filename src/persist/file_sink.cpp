#include "persist/file_sink.hpp"

#include <cerrno>
#include <cstring>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace persist {

PosixFileSink::PosixFileSink() = default;
PosixFileSink::~PosixFileSink() { close(); }

IoResult PosixFileSink::open(const std::string& path) noexcept {
    close();
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return {false, errno};
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        ::close(fd);
        return {false, err};
    }
    fd_ = fd;
    size_bytes_ = static_cast<std::uint64_t>(st.st_size);
    return {true, 0};
}

void PosixFileSink::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

IoResult PosixFileSink::writev(const struct iovec* iov, int iovcnt, std::size_t& bytes_written) noexcept {
    bytes_written = 0;
    if (fd_ < 0) {
        return {false, EBADF};
    }
    ssize_t ret = ::writev(fd_, iov, iovcnt);
    if (ret < 0) {
        return {false, errno};
    }
    bytes_written = static_cast<std::size_t>(ret);
    size_bytes_ += static_cast<std::uint64_t>(ret);
    return {true, 0};
}

bool PosixFileSink::is_open() const noexcept { return fd_ >= 0; }

#ifdef _WIN32
WindowsFileSink::WindowsFileSink() = default;
WindowsFileSink::~WindowsFileSink() { close(); }

IoResult WindowsFileSink::open(const std::string& path) noexcept {
    close();
    handle_ = ::CreateFileA(path.c_str(),
                            GENERIC_WRITE,
                            FILE_SHARE_READ,
                            nullptr,
                            OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        return {false, static_cast<int>(::GetLastError())};
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle_, &size)) {
        const int err = static_cast<int>(::GetLastError());
        close();
        return {false, err};
    }
    size_bytes_ = static_cast<std::uint64_t>(size.QuadPart);
    // Move to end for append semantics.
    ::SetFilePointer(handle_, 0, nullptr, FILE_END);
    return {true, 0};
}

void WindowsFileSink::close() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
}

IoResult WindowsFileSink::writev(const struct iovec* iov, int iovcnt, std::size_t& bytes_written) noexcept {
    bytes_written = 0;
    if (handle_ == INVALID_HANDLE_VALUE) {
        return {false, EBADF};
    }
    // Windows lacks writev; perform small buffered writes.
    for (int i = 0; i < iovcnt; ++i) {
        DWORD written = 0;
        if (!::WriteFile(handle_, iov[i].iov_base, static_cast<DWORD>(iov[i].iov_len), &written, nullptr)) {
            return {false, static_cast<int>(::GetLastError())};
        }
        bytes_written += written;
        size_bytes_ += written;
        if (written != iov[i].iov_len) {
            return {false, EIO};
        }
    }
    return {true, 0};
}

bool WindowsFileSink::is_open() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
#endif

} // namespace persist
