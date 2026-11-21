#pragma once

#include "dialects/sql_dialect.hpp"

namespace capybaradb::dialects {

[[nodiscard]] LexicalState scan_sqlite_line(
    std::string_view line,
    LexicalState initial_state,
    std::vector<TokenSpan>* out_tokens);

[[nodiscard]] bool is_sqlite_keyword(std::string_view word) noexcept;

} // namespace capybaradb::dialects
