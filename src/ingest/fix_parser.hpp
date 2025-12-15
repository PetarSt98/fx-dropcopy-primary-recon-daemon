#pragma once

#include <cstddef>
#include <cstdint>

#include "core/exec_event.hpp"

namespace ingest {

enum class ParseResult : uint8_t { Ok, MissingField, Invalid };

struct ParseStats {
    std::size_t parsed{0};
    std::size_t failed{0};
};

ParseResult parse_exec_report(const char* data, std::size_t len, core::ExecEvent& out) noexcept;

} // namespace ingest
