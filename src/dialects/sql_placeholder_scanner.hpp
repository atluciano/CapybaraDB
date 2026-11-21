#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace capybaradb::dialects {

struct UnboundParameterInfo {
    size_t byte_offset    = 0;
    size_t char_offset    = 0; // UTF-16 code-unit offset compatible with QTextCursor::setPosition
    int    first_number   = 0;
    int    highest_number = 0;
    int    count          = 0;

    [[nodiscard]] std::string description() const;
};

/// Scans PostgreSQL SQL statements separated by top-level ';' for unbound $n placeholders
/// (exempting PREPARE and CREATE [OR REPLACE] FUNCTION|PROCEDURE bodies) with zero heap allocations.
[[nodiscard]] std::optional<UnboundParameterInfo> find_postgres_unbound(std::string_view sql) noexcept;

} // namespace capybaradb::dialects
