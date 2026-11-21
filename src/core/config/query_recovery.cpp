#include "core/config/query_recovery.hpp"

#include "core/config/connection_profile.hpp"
#include "core/config/file_util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace capybaradb::core {
namespace {

constexpr size_t kMaxEncryptedFileBytes = 3ull * 1024u * 1024u; // 3 MiB

std::string compact_uuid(std::string_view uuid) {
    std::string out;
    out.reserve(32);
    for (const char ch : uuid) {
        if (ch == '-' || ch == '{' || ch == '}') {
            continue;
        }
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::string canonical_uuid(std::string_view uuid) {
    const std::string c = compact_uuid(uuid);
    if (c.size() != 32) {
        return {};
    }
    return c.substr(0, 8) + "-" + c.substr(8, 4) + "-" + c.substr(12, 4) + "-" + c.substr(16, 4) + "-" + c.substr(20, 12);
}

std::expected<void, std::string> reject_symlink(const std::filesystem::path& p) {
    std::error_code ec;
    const auto st = std::filesystem::symlink_status(p, ec);
    if (!ec && std::filesystem::is_symlink(st)) {
        return std::unexpected(std::string{"Recovery paths cannot be symbolic links."});
    }
    return {};
}

std::expected<std::filesystem::path, std::string> draft_path_for(
    const std::filesystem::path& dir,
    std::string_view id) {
    if (!parse_guid_bytes(id).has_value()) {
        return std::unexpected(std::string{"Recovery identity is missing."});
    }
    const std::string hex = compact_uuid(id);
    return dir / (hex + ".draft");
}

} // namespace

std::string RecoveryDraft::validate() const {
    if (!parse_guid_bytes(id).has_value()) {
        return "Recovery identity is missing or invalid.";
    }
    if (title.size() > kMaxTitleChars) {
        return "Recovery title exceeds 128 characters.";
    }
    if (text.size() > kMaxTextBytes) {
        return "Recovery supports up to 262144 bytes per unsaved query. Save larger queries to files.";
    }
    if (command_timeout_seconds < 0 || command_timeout_seconds > 86400) {
        return "Recovery timeout must be between 0 and 86400 seconds.";
    }
    return {};
}

std::vector<std::string> list_recovery_draft_ids(const std::filesystem::path& directory) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || ec) {
        return out;
    }
    if (!reject_symlink(directory).has_value()) {
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) continue;
        const auto& p = entry.path();
        if (p.extension() != ".draft") continue;
        const std::string stem = p.stem().string();
        if (stem.size() == 32 && parse_guid_bytes(stem).has_value()) {
            out.push_back(canonical_uuid(stem));
            if (out.size() >= RecoveryDraft::kMaxDocuments) {
                break;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::expected<void, std::string> save_recovery_draft(
    const std::filesystem::path& directory,
    const RecoveryDraft& draft) {
    if (const std::string err = draft.validate(); !err.empty()) {
        return std::unexpected(err);
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return std::unexpected(std::string{"Could not create recovery directory."});
    }
    if (auto ok = reject_symlink(directory); !ok) {
        return std::unexpected(ok.error());
    }

    auto path_res = draft_path_for(directory, draft.id);
    if (!path_res) {
        return std::unexpected(path_res.error());
    }
    const std::filesystem::path& target = *path_res;

    const bool exists = std::filesystem::exists(target, ec);
    if (!exists) {
        if (list_recovery_draft_ids(directory).size() >= RecoveryDraft::kMaxDocuments) {
            return std::unexpected(std::string{
                "Recovery supports up to 32 unsaved queries per window. Save some queries to files."});
        }
    } else {
        if (auto ok = reject_symlink(target); !ok) {
            return std::unexpected(ok.error());
        }
    }

    const std::string canon_id = canonical_uuid(draft.id);
    nlohmann::json envelope = {
        {"Version", 1},
        {"Draft", {
            {"Id", canon_id},
            {"Title", draft.title},
            {"Text", draft.text},
            {"Engine", draft.engine},
            {"CommandTimeoutSeconds", draft.command_timeout_seconds},
            {"WordWrap", draft.word_wrap},
            {"ShowWhiteSpace", draft.show_white_space},
            {"UpdatedUtcMs", draft.updated_utc_ms},
        }},
    };

    std::string clear = envelope.dump();
    const std::string encrypted = protect_secret(clear, canon_id);
    secure_wipe(clear);
    if (encrypted.empty()) {
        return std::unexpected(std::string{"Could not encrypt recovery draft."});
    }
    if (encrypted.size() > kMaxEncryptedFileBytes) {
        return std::unexpected(std::string{"Recovery record exceeds its size limit."});
    }

    const std::filesystem::path temp_path =
        target.parent_path() / (target.filename().string() + "." + compact_uuid(generate_uuid()) + ".partial");
    struct TempCleanup {
        std::filesystem::path p;
        bool active = true;
        ~TempCleanup() {
            if (active) {
                std::error_code rm_ec;
                std::filesystem::remove(p, rm_ec);
            }
        }
    } cleanup{temp_path};

    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return std::unexpected(std::string{"Could not open recovery temporary file."});
        }
        out.write(encrypted.data(), static_cast<std::streamsize>(encrypted.size()));
        out.flush();
        if (!out.good()) {
            return std::unexpected(std::string{"Failed writing recovery draft."});
        }
    }

    std::filesystem::rename(temp_path, target, ec);
    if (ec) {
        std::error_code rm_ec;
        std::filesystem::remove(target, rm_ec);
        ec.clear();
        std::filesystem::rename(temp_path, target, ec);
        if (ec) {
            return std::unexpected(std::string{"Could not finalize recovery draft file."});
        }
    }
    cleanup.active = false;
    return {};
}

std::expected<RecoveryDraft, std::string> read_recovery_draft(
    const std::filesystem::path& directory,
    std::string_view id) {
    if (auto ok = reject_symlink(directory); !ok) {
        return std::unexpected(ok.error());
    }
    auto path_res = draft_path_for(directory, id);
    if (!path_res) {
        return std::unexpected(path_res.error());
    }
    const std::filesystem::path& target = *path_res;
    if (auto ok = reject_symlink(target); !ok) {
        return std::unexpected(ok.error());
    }

    std::error_code ec;
    const auto sz = std::filesystem::file_size(target, ec);
    if (ec) {
        return std::unexpected(std::string{"Recovery record not found."});
    }
    if (sz > kMaxEncryptedFileBytes) {
        return std::unexpected(std::string{"Recovery record exceeds its size limit."});
    }

    const auto cipher = read_text_file(target);
    if (!cipher.has_value() || cipher->empty()) {
        return std::unexpected(std::string{"Recovery record could not be read."});
    }

    const std::string canon_id = canonical_uuid(id);
    auto clear = unprotect_secret(*cipher, canon_id);
    if (!clear.has_value()) {
        return std::unexpected(std::string{"Recovery record could not be decrypted for this user."});
    }

    const nlohmann::json j = nlohmann::json::parse(*clear, nullptr, false);
    secure_wipe(*clear);
    if (j.is_discarded() || !j.is_object() || j.value("Version", -1) != 1 ||
        !j.contains("Draft") || !j["Draft"].is_object()) {
        return std::unexpected(std::string{"Recovery record has an unsupported format."});
    }

    const auto& dj = j["Draft"];
    RecoveryDraft out;
    out.id                      = canonical_uuid(dj.value("Id", std::string{}));
    out.title                   = dj.value("Title", std::string{});
    out.text                    = dj.value("Text", std::string{});
    out.engine                  = dj.value("Engine", std::string{"PostgreSql"});
    out.command_timeout_seconds = dj.value("CommandTimeoutSeconds", 0);
    out.word_wrap               = dj.value("WordWrap", false);
    out.show_white_space        = dj.value("ShowWhiteSpace", false);
    out.updated_utc_ms          = dj.value("UpdatedUtcMs", int64_t{0});

    if (out.id != canon_id) {
        return std::unexpected(std::string{"Recovery record identity mismatch."});
    }
    if (const std::string err = out.validate(); !err.empty()) {
        return std::unexpected(err);
    }
    return out;
}

bool remove_recovery_draft(
    const std::filesystem::path& directory,
    std::string_view id) {
    if (!reject_symlink(directory).has_value()) {
        return false;
    }
    auto path_res = draft_path_for(directory, id);
    if (!path_res) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(*path_res, ec)) {
        return true;
    }
    return std::filesystem::remove(*path_res, ec) && !ec;
}

void clear_all_recovery_drafts(const std::filesystem::path& directory) {
    for (const std::string& id : list_recovery_draft_ids(directory)) {
        (void)remove_recovery_draft(directory, id);
    }
}

} // namespace capybaradb::core
