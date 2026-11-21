#pragma once

#include <filesystem>

namespace capybaradb::core {

/// Resolves the application state directory using CAPYBARADB_STATE_DIRECTORY when set,
/// otherwise %LOCALAPPDATA%\CapybaraDB on Windows or $XDG_CONFIG_HOME/capybaradb (~/.config/capybaradb) on Linux.
[[nodiscard]] std::filesystem::path state_directory();

[[nodiscard]] std::filesystem::path environment_options_path();
[[nodiscard]] std::filesystem::path explorer_toolbar_prefs_path();
[[nodiscard]] std::filesystem::path dock_layout_path();
[[nodiscard]] std::filesystem::path connections_path();
[[nodiscard]] std::filesystem::path connect_dialog_prefs_path();
[[nodiscard]] std::filesystem::path query_history_path();
[[nodiscard]] std::filesystem::path recovery_directory();

} // namespace capybaradb::core
