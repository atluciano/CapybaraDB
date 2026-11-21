#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace capybaradb::core {

struct RecoveryDraft {
    static constexpr size_t kMaxTitleChars = 128u;
    static constexpr size_t kMaxTextBytes  = 262144u; // 256 KiB per unsaved query
    static constexpr size_t kMaxDocuments  = 32u;

    std::string id; // UUID string (entropy for DPAPI / SecretStore)
    std::string title;
    std::string text;
    std::string engine = "PostgreSql";
    int32_t     command_timeout_seconds = 0;
    bool        word_wrap               = false;
    bool        show_white_space        = false;
    int64_t     updated_utc_ms          = 0;

    [[nodiscard]] std::string validate() const;
};

/// One encrypted `.draft` file per unsaved query tab, protected with DPAPI / libsecret
/// using the draft's UUID as entropy, written atomically via `.partial` + rename.
[[nodiscard]] std::vector<std::string> list_recovery_draft_ids(const std::filesystem::path& directory);

[[nodiscard]] std::expected<void, std::string> save_recovery_draft(
    const std::filesystem::path& directory,
    const RecoveryDraft& draft);

[[nodiscard]] std::expected<RecoveryDraft, std::string> read_recovery_draft(
    const std::filesystem::path& directory,
    std::string_view id);

bool remove_recovery_draft(
    const std::filesystem::path& directory,
    std::string_view id);

void clear_all_recovery_drafts(const std::filesystem::path& directory);

} // namespace capybaradb::core
