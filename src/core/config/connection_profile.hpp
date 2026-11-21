#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace capybaradb::core {

// ---------------------------------------------------------------------------
// Platform secret protection and GUID helpers (free functions, no static class)
// ---------------------------------------------------------------------------

[[nodiscard]] std::string generate_uuid();

/// Parses a UUID string into a 16-byte binary layout
/// (first 3 fields little-endian, last 8 bytes big-endian) for DPAPI entropy compatibility.
[[nodiscard]] std::optional<std::array<uint8_t, 16>> parse_guid_bytes(
    std::string_view uuid_str) noexcept;

[[nodiscard]] std::string protect_secret(
    std::string_view plaintext,
    std::string_view profile_guid_str);

[[nodiscard]] std::optional<std::string> unprotect_secret(
    std::string_view encoded_ciphertext,
    std::string_view profile_guid_str);

void secure_wipe(std::string& buffer) noexcept;
void secure_wipe(std::vector<uint8_t>& buffer) noexcept;

[[nodiscard]] std::string base64_encode(std::span<const uint8_t> bytes);
[[nodiscard]] std::optional<std::vector<uint8_t>> base64_decode(std::string_view encoded);

// ---------------------------------------------------------------------------
// ConnectionProfile & Server Group persistence (zero exceptions, no Store class)
// ---------------------------------------------------------------------------

struct ConnectionProfile {
    std::string                id;
    std::string                name;
    std::string                engine    = "PostgreSql"; // "PostgreSql" | "SqlServer" | "SQLite"
    std::string                host      = "localhost";
    int                        port      = 5432;
    std::string                database  = "postgres";
    std::string                username  = "postgres";
    std::string                auth_mode = "Password";   // "Password" | "Integrated"
    std::string                ssl_mode  = "VerifyFull";
    std::optional<std::string> protected_password;
    std::optional<std::string> root_certificate;
    std::optional<std::string> group;
    bool                       disable_query_history = false;

    /// Returns empty string when valid, otherwise a user-facing validation error message.
    [[nodiscard]] std::string validate() const;

    /// Unprotects protected_password using the profile GUID as entropy. Returns std::nullopt on error.
    [[nodiscard]] std::optional<std::string> reveal_password() const;

    /// Mutates protected_password in-place (zero struct copy).
    void set_password(std::string_view password, bool remember);
};

[[nodiscard]] std::string validate_server_group(std::string_view path);

/// Loads saved profiles sorted case-insensitively by name without throwing exceptions.
/// On error or invalid JSON, returns an empty vector and populates `error_out` when non-null.
[[nodiscard]] std::vector<ConnectionProfile> load_connection_profiles(
    const std::filesystem::path& path,
    std::string* error_out = nullptr);

[[nodiscard]] std::vector<std::string> load_connection_profile_groups(
    const std::filesystem::path& path,
    std::string* error_out = nullptr);

bool save_connection_profile(
    const std::filesystem::path& path,
    const ConnectionProfile& profile,
    std::string* error_out = nullptr);

bool remove_connection_profile(
    const std::filesystem::path& path,
    std::string_view profile_id,
    std::string* error_out = nullptr);

bool add_connection_profile_group(
    const std::filesystem::path& path,
    std::string_view group_path,
    std::string* error_out = nullptr);

bool rename_connection_profile_group(
    const std::filesystem::path& path,
    std::string_view old_path,
    std::string_view new_path,
    std::string* error_out = nullptr);

bool remove_connection_profile_group(
    const std::filesystem::path& path,
    std::string_view group_path,
    std::string* error_out = nullptr);

/// Returns true if any saved profile matching (engine, host, port) or database disables query history.
[[nodiscard]] bool is_query_history_disabled_for_connection(
    const std::filesystem::path& path,
    std::string_view engine,
    std::string_view host,
    int port,
    std::string_view database = {});

// ---------------------------------------------------------------------------
// ConnectionDialogPreferences persistence (no std::function mutator)
// ---------------------------------------------------------------------------

struct ConnectionDialogPreferences {
    static constexpr int kCurrentVersion = 1;

    int                        version      = kCurrentVersion;
    std::optional<std::string> last_profile;
    std::optional<double>      width;
    std::optional<double>      height;
};

[[nodiscard]] ConnectionDialogPreferences load_connection_dialog_preferences(
    const std::filesystem::path& path);

bool save_connection_dialog_preferences(
    const std::filesystem::path& path,
    const ConnectionDialogPreferences& prefs);

} // namespace capybaradb::core
