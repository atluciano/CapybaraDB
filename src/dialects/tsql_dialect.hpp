#pragma once

#include "dialects/sql_dialect.hpp"

namespace capybaradb::dialects {

[[nodiscard]] LexicalState scan_tsql_line(
    std::string_view line,
    LexicalState initial_state,
    std::vector<TokenSpan>* out_tokens);

[[nodiscard]] bool is_tsql_keyword(std::string_view word) noexcept;

} // namespace capybaradb::dialects
