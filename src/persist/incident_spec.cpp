#include "persist/incident_spec.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>

namespace persist {
namespace {

class JsonCursor {
public:
    explicit JsonCursor(std::string_view s) : src_(s) {}

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
        ++pos_; // skip opening quote
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

    bool parse_literal(std::string_view literal, std::string& err) noexcept {
        skip_ws();
        if (src_.substr(pos_).compare(0, literal.size(), literal) == 0) {
            pos_ += literal.size();
            return true;
        }
        err = "Expected literal";
        return false;
    }

    bool eof() const noexcept {
        skip_ws();
        return pos_ >= src_.size();
    }

private:
    mutable std::size_t pos_{0};
    std::string_view src_;
};

std::optional<core::OrderKey> hash_clordid(std::string_view text) noexcept {
    // Same FNV-1a 64-bit hash as core::make_order_key.
    static constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ULL;
    static constexpr std::uint64_t fnv_prime = 1099511628211ULL;

    std::uint64_t hash = fnv_offset_basis;
    for (char c : text) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= fnv_prime;
    }
    return hash;
}

} // namespace

std::optional<core::DivergenceType> divergence_type_from_string(std::string_view s) noexcept {
    if (s == "MissingFill") return core::DivergenceType::MissingFill;
    if (s == "PhantomOrder") return core::DivergenceType::PhantomOrder;
    if (s == "StateMismatch") return core::DivergenceType::StateMismatch;
    if (s == "QuantityMismatch") return core::DivergenceType::QuantityMismatch;
    if (s == "TimingAnomaly") return core::DivergenceType::TimingAnomaly;
    return std::nullopt;
}

std::optional<core::OrderKey> parse_order_key_literal(std::string_view literal) noexcept {
    if (literal.rfind("hash:", 0) == 0) {
        std::uint64_t value = 0;
        const auto numeric = literal.substr(5);
        auto res = std::from_chars(numeric.data(), numeric.data() + numeric.size(), value);
        if (res.ec == std::errc()) {
            return value;
        }
        return std::nullopt;
    }
    if (literal.rfind("clordid:", 0) == 0) {
        return hash_clordid(literal.substr(8));
    }
    // Backward-compatible alias: hash the entire literal.
    return hash_clordid(literal);
}

bool wildcard_match(std::string_view pattern, std::string_view value) noexcept {
    std::size_t p = 0, v = 0, star = std::string_view::npos, match = 0;
    while (v < value.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' || pattern[p] == value[v])) {
            ++p; ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = v;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            v = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

static bool load_file(const std::filesystem::path& path, std::string& out, std::string& error) noexcept {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Failed to open file: " + path.string();
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto len = in.tellg();
    if (len < 0) {
        error = "Failed to size file: " + path.string();
        return false;
    }
    out.resize(static_cast<std::size_t>(len));
    in.seekg(0, std::ios::beg);
    if (!in.read(out.data(), out.size())) {
        error = "Failed to read file: " + path.string();
        return false;
    }
    return true;
}

bool parse_incident_spec(const std::filesystem::path& path,
                         IncidentSpec& out,
                         std::string& error) noexcept {
    std::string contents;
    if (!load_file(path, contents, error)) {
        return false;
    }
    JsonCursor cur(contents);
    if (!cur.expect('{')) {
        error = "Expected object";
        return false;
    }
    bool seen_id = false;
    bool seen_desc = false;
    bool seen_wire = false;
    bool seen_replay = false;
    IncidentSpec spec;
    while (true) {
        cur.skip_ws();
        if (cur.consume('}')) {
            break;
        }
        std::string key_err;
        auto key = cur.parse_string(key_err);
        if (!key) { error = key_err; return false; }
        if (!cur.expect(':')) { error = "Expected ':'"; return false; }
        if (*key == "id") {
            auto val = cur.parse_string(key_err);
            if (!val) { error = key_err; return false; }
            spec.id = *val;
            seen_id = true;
        } else if (*key == "description") {
            auto val = cur.parse_string(key_err);
            if (!val) { error = key_err; return false; }
            spec.description = *val;
            seen_desc = true;
        } else if (*key == "wire_inputs") {
            if (!cur.expect('[')) { error = "Expected array"; return false; }
            while (true) {
                cur.skip_ws();
                if (cur.consume(']')) { break; }
                if (!cur.expect('{')) { error = "Expected object"; return false; }
                IncidentWireInput wi;
                bool pth=false, f=false, t=false;
                while (true) {
                    std::string kerr;
                    auto wkey = cur.parse_string(kerr);
                    if (!wkey) { error = kerr; return false; }
                    if (!cur.expect(':')) { error = "Expected ':'"; return false; }
                    if (*wkey == "path") {
                        auto v = cur.parse_string(kerr);
                        if (!v) { error = kerr; return false; }
                        wi.path = *v;
                        pth = true;
                    } else if (*wkey == "from_ns") {
                        auto v = cur.parse_uint64(kerr);
                        if (!v) { error = kerr; return false; }
                        wi.from_ns = *v;
                        f = true;
                    } else if (*wkey == "to_ns") {
                        auto v = cur.parse_uint64(kerr);
                        if (!v) { error = kerr; return false; }
                        wi.to_ns = *v;
                        t = true;
                    } else {
                        error = "Unknown wire_inputs field: " + *wkey;
                        return false;
                    }
                    cur.skip_ws();
                    if (cur.consume('}')) { break; }
                    if (!cur.consume(',')) { error = "Expected ','"; return false; }
                }
                if (!(pth && f && t)) { error = "wire_inputs entry missing fields"; return false; }
                spec.wire_inputs.push_back(wi);
                cur.skip_ws();
                if (cur.consume(']')) { break; }
                if (!cur.consume(',')) { error = "Expected ','"; return false; }
            }
            seen_wire = true;
        } else if (*key == "replay") {
            if (!cur.expect('{')) { error = "Expected object"; return false; }
            while (true) {
                cur.skip_ws();
                if (cur.consume('}')) break;
                std::string rkerr;
                auto rkey = cur.parse_string(rkerr);
                if (!rkey) { error = rkerr; return false; }
                if (!cur.expect(':')) { error = "Expected ':'"; return false; }
                if (*rkey == "speed") {
                    auto v = cur.parse_string(rkerr);
                    if (!v) { error = rkerr; return false; }
                    spec.replay.speed = *v;
                } else if (*rkey == "max_records") {
                    auto v = cur.parse_uint64(rkerr);
                    if (!v) { error = rkerr; return false; }
                    spec.replay.max_records = *v;
                } else {
                    error = "Unknown replay field: " + *rkey;
                    return false;
                }
                cur.skip_ws();
                if (cur.consume('}')) break;
                if (!cur.consume(',')) { error = "Expected ','"; return false; }
            }
            seen_replay = true;
        } else {
            error = "Unknown field: " + *key;
            return false;
        }
        cur.skip_ws();
        if (cur.consume('}')) break;
        if (!cur.consume(',')) { error = "Expected ','"; return false; }
    }
    if (!(seen_id && seen_desc && seen_wire && seen_replay)) {
        error = "Missing required fields";
        return false;
    }
    spec.replay.speed = spec.replay.speed.empty() ? "fast" : spec.replay.speed;
    out = std::move(spec);
    return true;
}

static bool parse_rules_array(JsonCursor& cur, Whitelist& wl, std::string& error) noexcept {
    if (!cur.expect('[')) { error = "Expected rules array"; return false; }
    while (true) {
        cur.skip_ws();
        if (cur.consume(']')) { break; }
        if (!cur.expect('{')) { error = "Expected rule object"; return false; }
        WhitelistRule rule;
        bool have_type = false;
        bool done = false;
        while (!done) {
            std::string kerr;
            auto key = cur.parse_string(kerr);
            if (!key) { error = kerr; return false; }
            if (!cur.expect(':')) { error = "Expected ':'"; return false; }
            if (*key == "type") {
                auto v = cur.parse_string(kerr);
                if (!v) { error = kerr; return false; }
                if (*v == "ignore_divergence_type") {
                    rule.type = WhitelistRuleType::IgnoreDivergenceType;
                } else if (*v == "ignore_n_occurrences") {
                    rule.type = WhitelistRuleType::IgnoreNOccurrences;
                } else if (*v == "ignore_by_order_key") {
                    rule.type = WhitelistRuleType::IgnoreByOrderKey;
                } else if (*v == "allow_extra_files") {
                    rule.type = WhitelistRuleType::AllowExtraFiles;
                } else {
                    error = "Unknown rule type: " + *v;
                    return false;
                }
                have_type = true;
            } else if (*key == "divergence_type") {
                auto v = cur.parse_string(kerr);
                if (!v) { error = kerr; return false; }
                auto maybe = divergence_type_from_string(*v);
                if (!maybe) { error = "Unknown divergence_type: " + *v; return false; }
                rule.divergence_type = *maybe;
            } else if (*key == "max_occurrences") {
                auto v = cur.parse_uint64(kerr);
                if (!v) { error = kerr; return false; }
                rule.remaining_occurrences = static_cast<std::size_t>(*v);
            } else if (*key == "order_keys") {
                if (!cur.expect('[')) { error = "Expected order_keys array"; return false; }
                while (true) {
                    cur.skip_ws();
                    if (cur.consume(']')) break;
                    auto sv = cur.parse_string(kerr);
                    if (!sv) { error = kerr; return false; }
                    auto key_val = parse_order_key_literal(*sv);
                    if (!key_val) { error = "Invalid order key literal: " + *sv; return false; }
                    rule.order_keys.push_back(*key_val);
                    cur.skip_ws();
                    if (cur.consume(']')) break;
                    if (!cur.consume(',')) { error = "Expected ','"; return false; }
                }
            } else if (*key == "patterns") {
                if (!cur.expect('[')) { error = "Expected patterns array"; return false; }
                while (true) {
                    cur.skip_ws();
                    if (cur.consume(']')) break;
                    auto sv = cur.parse_string(kerr);
                    if (!sv) { error = kerr; return false; }
                    rule.patterns.push_back(*sv);
                    cur.skip_ws();
                    if (cur.consume(']')) break;
                    if (!cur.consume(',')) { error = "Expected ','"; return false; }
                }
            } else if (*key == "reason") {
                auto v = cur.parse_string(kerr);
                if (!v) { error = kerr; return false; }
                rule.reason = *v;
            } else if (*key == "venue") {
                auto v = cur.parse_string(kerr);
                if (!v) { error = kerr; return false; }
                rule.venue = *v;
            } else {
                error = "Unknown field in rule: " + *key;
                return false;
            }
            cur.skip_ws();
            if (cur.consume('}')) {
                done = true;
            } else if (!cur.consume(',')) {
                error = "Expected ','";
                return false;
            }
        }
        if (!have_type) { error = "Rule missing type"; return false; }
        wl.rules.push_back(std::move(rule));
        cur.skip_ws();
        if (cur.consume(']')) break;
        if (!cur.consume(',')) { error = "Expected ','"; return false; }
    }
    return true;
}

bool parse_whitelist(const std::filesystem::path& path,
                     Whitelist& out,
                     std::string& error) noexcept {
    std::string contents;
    if (!load_file(path, contents, error)) {
        return false;
    }
    JsonCursor cur(contents);
    if (!cur.expect('{')) { error = "Expected object"; return false; }
    Whitelist wl;
    bool seen_version = false;
    bool seen_rules = false;
    while (true) {
        cur.skip_ws();
        if (cur.consume('}')) break;
        std::string kerr;
        auto key = cur.parse_string(kerr);
        if (!key) { error = kerr; return false; }
        if (!cur.expect(':')) { error = "Expected ':'"; return false; }
        if (*key == "version") {
            auto v = cur.parse_uint64(kerr);
            if (!v) { error = kerr; return false; }
            wl.version = static_cast<int>(*v);
            seen_version = true;
        } else if (*key == "rules") {
            if (!parse_rules_array(cur, wl, error)) { return false; }
            seen_rules = true;
        } else {
            error = "Unknown field: " + *key;
            return false;
        }
        cur.skip_ws();
        if (cur.consume('}')) break;
        if (!cur.consume(',')) { error = "Expected ','"; return false; }
    }
    if (!seen_version || !seen_rules) {
        error = "Missing version or rules";
        return false;
    }
    out = std::move(wl);
    return true;
}

} // namespace persist
