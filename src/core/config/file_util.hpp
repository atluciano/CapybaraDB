#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace capybaradb::core {

/// Atomically writes UTF-8 content via a sibling .tmp file and rename.
[[nodiscard]] bool atomic_write_utf8(
    const std::filesystem::path& target_path,
    const std::string& content);

/// Reads a UTF-8 file up to 16 MiB, returning std::nullopt if absent or unreadable.
[[nodiscard]] std::optional<std::string> read_text_file(
    const std::filesystem::path& path);

} // namespace capybaradb::core
