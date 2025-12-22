#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/divergence.hpp"
#include "core/order_state.hpp"

namespace persist {

struct IncidentWireInput {
    std::filesystem::path path;
    std::uint64_t from_ns{0};
    std::uint64_t to_ns{0};
};

struct ReplayOptions {
    std::string speed{"fast"};
    std::uint64_t max_records{0};
};

struct IncidentSpec {
    std::string id;
    std::string description;
    std::vector<IncidentWireInput> wire_inputs;
    ReplayOptions replay;
};

enum class WhitelistRuleType {
    IgnoreDivergenceType,
    IgnoreNOccurrences,
    IgnoreByOrderKey,
    AllowExtraFiles
};

struct WhitelistRule {
    WhitelistRuleType type{WhitelistRuleType::IgnoreDivergenceType};
    // For IgnoreDivergenceType and IgnoreNOccurrences.
    core::DivergenceType divergence_type{core::DivergenceType::StateMismatch};
    std::size_t remaining_occurrences{0}; // Used only for IgnoreNOccurrences.
    // For IgnoreByOrderKey.
    std::vector<core::OrderKey> order_keys;
    // For AllowExtraFiles.
    std::vector<std::string> patterns;
    // Optional venue label (ignored if not present in payload).
    std::string venue;
    std::string reason;
};

struct Whitelist {
    int version{1};
    std::vector<WhitelistRule> rules;

    bool empty() const noexcept { return rules.empty(); }
};

// Parsing helpers. These functions are schema-specific and deterministic;
// they avoid dynamic locale dependencies and do not throw.
bool parse_incident_spec(const std::filesystem::path& path,
                         IncidentSpec& out,
                         std::string& error) noexcept;

bool parse_whitelist(const std::filesystem::path& path,
                     Whitelist& out,
                     std::string& error) noexcept;

// Shared helpers exposed for testing and reuse.
std::optional<core::DivergenceType> divergence_type_from_string(std::string_view s) noexcept;

// Hash an order key literal using the same FNV-1a algorithm as audit writer.
// Accepts prefixes:
//   - "hash:<uint64>" uses the numeric literal directly.
//   - "clordid:<text>" hashes the text.
//   - Legacy strings (e.g., "EBS:12345") are hashed as-is for compatibility.
std::optional<core::OrderKey> parse_order_key_literal(std::string_view literal) noexcept;

// Simple glob matcher supporting '*' and '?'.
bool wildcard_match(std::string_view pattern, std::string_view value) noexcept;

} // namespace persist
