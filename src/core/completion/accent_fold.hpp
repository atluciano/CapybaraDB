#pragma once

#include <string>
#include <string_view>

namespace capybaradb::core {

/// Folds UTF-8 text to lowercase ASCII with Latin diacritics stripped (e.g. "Usuários" -> "usuarios", "Ação" -> "acao").
[[nodiscard]] std::string fold_accents_lower(std::string_view utf8);

/// Returns true if `haystack` starts with `needle` after accent-insensitive lowercase folding.
[[nodiscard]] bool accent_starts_with(std::string_view haystack, std::string_view needle);

/// Returns true if `haystack` contains `needle` after accent-insensitive lowercase folding.
[[nodiscard]] bool accent_contains(std::string_view haystack, std::string_view needle);

} // namespace capybaradb::core
