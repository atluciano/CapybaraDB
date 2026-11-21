#include "drivers/mssql/mssql_batch_splitter.hpp"

#include <cctype>
#include <charconv>

namespace capybaradb::drivers {
namespace {

enum class ScanState {
    Normal,
    SingleQuoted,
    BracketIdent,
    DoubleQuoted,
    BlockComment,
};

struct GoMatch {
    bool     matched = false;
    uint32_t count   = 1;
};

GoMatch parse_go_line(std::string_view line) noexcept {
    size_t i = 0;
    const size_t n = line.size();
    while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
        ++i;
    }
    if (i + 2 > n) {
        return {};
    }
    const char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(line[i])));
    const char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(line[i + 1])));
    if (c0 != 'g' || c1 != 'o') {
        return {};
    }
    i += 2;
    if (i < n && !std::isspace(static_cast<unsigned char>(line[i])) && !(line[i] == '-' && i + 1 < n && line[i + 1] == '-')) {
        return {};
    }
    while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
        ++i;
    }

    uint32_t count = 1;
    if (i < n && std::isdigit(static_cast<unsigned char>(line[i]))) {
        size_t start = i;
        while (i < n && std::isdigit(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        uint32_t parsed = 1;
        const auto [ptr, ec] = std::from_chars(line.data() + start, line.data() + i, parsed);
        if (ec != std::errc{} || ptr != line.data() + i || parsed == 0) {
            return {};
        }
        count = parsed;
        while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
            ++i;
        }
    }

    if (i < n) {
        if (line[i] == '-' && i + 1 < n && line[i + 1] == '-') {
            return GoMatch{true, count};
        }
        return {};
    }
    return GoMatch{true, count};
}

std::string trim_copy(std::string_view text) {
    size_t a = 0;
    while (a < text.size() && std::isspace(static_cast<unsigned char>(text[a]))) {
        ++a;
    }
    size_t b = text.size();
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) {
        --b;
    }
    return std::string(text.substr(a, b - a));
}

} // namespace

std::vector<TSqlBatch> split_tsql_batches(std::string_view script) {
    std::vector<TSqlBatch> batches;
    ScanState state = ScanState::Normal;
    int block_depth = 0;

    size_t batch_start = 0;
    uint32_t batch_start_line = 1;
    uint32_t cur_line = 1;

    size_t pos = 0;
    while (pos < script.size()) {
        const size_t nl = script.find('\n', pos);
        const size_t line_end = (nl == std::string_view::npos) ? script.size() : nl;
        const size_t next_pos = (nl == std::string_view::npos) ? script.size() : nl + 1;
        const std::string_view line = script.substr(pos, line_end - pos);

        bool check_go = (state == ScanState::Normal);
        GoMatch go{};
        if (check_go) {
            go = parse_go_line(line);
        }

        if (go.matched) {
            std::string body = trim_copy(script.substr(batch_start, pos - batch_start));
            if (!body.empty()) {
                batches.push_back(TSqlBatch{std::move(body), go.count, batch_start_line});
            }
            batch_start = next_pos;
            batch_start_line = cur_line + 1;
        } else {
            // Advance lexical state across the characters on this line.
            for (size_t i = 0; i < line.size(); ++i) {
                const char ch = line[i];
                const char nx = (i + 1 < line.size()) ? line[i + 1] : '\0';

                switch (state) {
                case ScanState::Normal:
                    if (ch == '-' && nx == '-') {
                        i = line.size();
                    } else if (ch == '/' && nx == '*') {
                        state = ScanState::BlockComment;
                        block_depth = 1;
                        ++i;
                    } else if (ch == '\'') {
                        state = ScanState::SingleQuoted;
                    } else if (ch == '[') {
                        state = ScanState::BracketIdent;
                    } else if (ch == '"') {
                        state = ScanState::DoubleQuoted;
                    }
                    break;
                case ScanState::SingleQuoted:
                    if (ch == '\'') {
                        if (nx == '\'') {
                            ++i;
                        } else {
                            state = ScanState::Normal;
                        }
                    }
                    break;
                case ScanState::BracketIdent:
                    if (ch == ']') {
                        if (nx == ']') {
                            ++i;
                        } else {
                            state = ScanState::Normal;
                        }
                    }
                    break;
                case ScanState::DoubleQuoted:
                    if (ch == '"') {
                        if (nx == '"') {
                            ++i;
                        } else {
                            state = ScanState::Normal;
                        }
                    }
                    break;
                case ScanState::BlockComment:
                    if (ch == '/' && nx == '*') {
                        ++block_depth;
                        ++i;
                    } else if (ch == '*' && nx == '/') {
                        --block_depth;
                        if (block_depth <= 0) {
                            state = ScanState::Normal;
                            block_depth = 0;
                        }
                        ++i;
                    }
                    break;
                }
            }
        }

        pos = next_pos;
        ++cur_line;
    }

    if (batch_start < script.size()) {
        std::string tail = trim_copy(script.substr(batch_start));
        if (!tail.empty()) {
            batches.push_back(TSqlBatch{std::move(tail), 1, batch_start_line});
        }
    }

    return batches;
}

} // namespace capybaradb::drivers
