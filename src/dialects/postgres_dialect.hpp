#pragma once

#include "dialects/sql_dialect.hpp"

namespace capybaradb::dialects {

[[nodiscard]] LexicalState scan_postgres_line(
    std::string_view line,
    LexicalState initial_state,
    std::vector<TokenSpan>* out_tokens);

[[nodiscard]] bool is_postgres_keyword(std::string_view word) noexcept;

} // namespace capybaradb::dialects
