#pragma once

#include <string>

namespace util {
inline std::string pipe_to_soh(const std::string& msg) {
    std::string out = msg;
    for (char& c : out) {
        if (c == '|') c = '\x01';
    }
    return out;
}
} // namespace util
