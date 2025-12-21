#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/uio.h>
#else
struct iovec {
    void* iov_base;
    std::size_t iov_len;
};
#endif

namespace persist {

struct IoResult {
    bool ok{false};
    int error_code{0};
};

class IFileSink {
public:
    virtual ~IFileSink() = default;
    virtual IoResult open(const std::string& path) noexcept = 0;
    virtual void close() noexcept = 0;
    virtual IoResult writev(const struct iovec* iov, int iovcnt, std::size_t& bytes_written) noexcept = 0;
    virtual std::uint64_t current_size() const noexcept = 0;
    virtual bool is_open() const noexcept = 0;
};

class PosixFileSink : public IFileSink {
public:
    PosixFileSink();
    ~PosixFileSink() override;

    IoResult open(const std::string& path) noexcept override;
    void close() noexcept override;
    IoResult writev(const struct iovec* iov, int iovcnt, std::size_t& bytes_written) noexcept override;
    std::uint64_t current_size() const noexcept override { return size_bytes_; }
    bool is_open() const noexcept override;

private:
    int fd_{-1};
    std::uint64_t size_bytes_{0};
};

#ifdef _WIN32
#include <Windows.h>
class WindowsFileSink : public IFileSink {
public:
    WindowsFileSink();
    ~WindowsFileSink() override;

    IoResult open(const std::string& path) noexcept override;
    void close() noexcept override;
    IoResult writev(const struct iovec* iov, int iovcnt, std::size_t& bytes_written) noexcept override;
    std::uint64_t current_size() const noexcept override { return size_bytes_; }
    bool is_open() const noexcept override;

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::uint64_t size_bytes_{0};
};
#endif

} // namespace persist
