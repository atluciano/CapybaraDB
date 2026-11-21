#include "core/config/connection_profile.hpp"

#include "core/config/file_util.hpp"
#include "drivers/common/driver_ui_descriptor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <nlohmann/json.hpp>

namespace capybaradb::core {
namespace {

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

std::string trim_ascii(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string(s.substr(start, end - start));
}

int hex_nibble(char ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

const nlohmann::json* find_field(const nlohmann::json& obj, const char* pascal, const char* camel) {
    if (const auto it = obj.find(pascal); it != obj.end()) {
        return &(*it);
    }
    if (const auto it = obj.find(camel); it != obj.end()) {
        return &(*it);
    }
    return nullptr;
}

std::string read_str_field(const nlohmann::json& obj, const char* pascal, const char* camel, std::string def = {}) {
    if (const nlohmann::json* val = find_field(obj, pascal, camel); val != nullptr && val->is_string()) {
        return val->get<std::string>();
    }
    return def;
}

std::optional<std::string> read_opt_str_field(const nlohmann::json& obj, const char* pascal, const char* camel) {
    if (const nlohmann::json* val = find_field(obj, pascal, camel); val != nullptr && val->is_string()) {
        std::string s = val->get<std::string>();
        if (!s.empty()) {
            return s;
        }
    }
    return std::nullopt;
}

int read_int_field(const nlohmann::json& obj, const char* pascal, const char* camel, int def) {
    if (const nlohmann::json* val = find_field(obj, pascal, camel); val != nullptr && val->is_number_integer()) {
        return val->get<int>();
    }
    return def;
}

bool read_bool_field(const nlohmann::json& obj, const char* pascal, const char* camel, bool def) {
    if (const nlohmann::json* val = find_field(obj, pascal, camel); val != nullptr && val->is_boolean()) {
        return val->get<bool>();
    }
    return def;
}

struct StoredDocument {
    int                            version = 1;
    std::vector<ConnectionProfile> profiles;
    std::vector<std::string>       groups;
};

bool read_document(const std::filesystem::path& path, StoredDocument& out_doc, std::string* error_out) {
    out_doc = StoredDocument{};
    if (error_out != nullptr) {
        error_out->clear();
    }
    const auto fail = [&](std::string msg) -> bool {
        if (error_out != nullptr) {
            *error_out = std::move(msg);
        }
        return false;
    };

    const auto text = read_text_file(path);
    if (!text.has_value()) {
        return true;
    }

    const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return fail("The profile file is empty or invalid.");
    }

    const int ver = read_int_field(j, "Version", "version", -1);
    const nlohmann::json* profiles_arr = find_field(j, "Profiles", "profiles");
    if (ver != 1 || profiles_arr == nullptr || !profiles_arr->is_array()) {
        return fail("Unsupported connection profile file version.");
    }

    StoredDocument doc;
    doc.version = 1;

    std::vector<std::string> seen_ids;
    for (const auto& item : *profiles_arr) {
        if (!item.is_object()) {
            return fail("Invalid connection profile entry.");
        }
        ConnectionProfile p;
        p.id       = to_lower_ascii(trim_ascii(read_str_field(item, "Id", "id")));
        p.name     = read_str_field(item, "Name", "name");
        p.engine   = read_str_field(item, "Engine", "engine", "PostgreSql");
        const auto eng_kind = drivers::engine_kind_from_string(p.engine);
        const auto& desc    = drivers::driver_ui_descriptor(eng_kind);
        p.host     = read_str_field(item, "Host", "host",
                                    desc.storage_kind == drivers::ConnectionStorageKind::LocalFile ? "localhost" : "");
        p.port     = read_int_field(item, "Port", "port", static_cast<int>(desc.default_port));
        p.database = read_str_field(item, "Database", "database");
        p.username = read_str_field(item, "Username", "username");
        p.auth_mode = read_str_field(item, "AuthMode", "auth_mode", "Password");
        p.ssl_mode  = read_str_field(item, "SslMode", "ssl_mode", "VerifyFull");
        p.protected_password = read_opt_str_field(item, "ProtectedPassword", "protected_password");
        p.root_certificate   = read_opt_str_field(item, "RootCertificate", "root_certificate");
        p.group              = read_opt_str_field(item, "Group", "group");
        p.disable_query_history = read_bool_field(item, "DisableQueryHistory", "disable_query_history", false);

        if (const std::string err = p.validate(); !err.empty()) {
            return fail(err);
        }
        if (std::find(seen_ids.begin(), seen_ids.end(), p.id) != seen_ids.end()) {
            return fail("Duplicate profile identities.");
        }
        seen_ids.push_back(p.id);
        doc.profiles.push_back(std::move(p));
    }

    if (const nlohmann::json* groups_arr = find_field(j, "Groups", "groups"); groups_arr != nullptr) {
        if (!groups_arr->is_array()) {
            return fail("Invalid connection profile group entry.");
        }
        for (const auto& g : *groups_arr) {
            if (!g.is_string()) {
                return fail("Invalid connection profile group entry.");
            }
            const std::string gstr = g.get<std::string>();
            if (const std::string err = validate_server_group(gstr); !err.empty()) {
                return fail(err);
            }
            doc.groups.push_back(gstr);
        }
    }

    std::sort(doc.profiles.begin(), doc.profiles.end(), [](const ConnectionProfile& a, const ConnectionProfile& b) {
        return to_lower_ascii(a.name) < to_lower_ascii(b.name);
    });
    out_doc = std::move(doc);
    return true;
}

bool write_document(const std::filesystem::path& path, StoredDocument doc) {
    std::sort(doc.profiles.begin(), doc.profiles.end(), [](const ConnectionProfile& a, const ConnectionProfile& b) {
        return to_lower_ascii(a.name) < to_lower_ascii(b.name);
    });

    nlohmann::json profiles_json = nlohmann::json::array();
    for (const auto& p : doc.profiles) {
        nlohmann::json entry = {
            {"Id", p.id},
            {"Name", p.name},
            {"Engine", p.engine},
            {"Host", p.host},
            {"Port", p.port},
            {"Database", p.database},
            {"Username", p.username},
            {"AuthMode", p.auth_mode},
            {"SslMode", p.ssl_mode},
            {"ProtectedPassword", p.protected_password.has_value() ? nlohmann::json(*p.protected_password) : nlohmann::json(nullptr)},
            {"RootCertificate", p.root_certificate.has_value() ? nlohmann::json(*p.root_certificate) : nlohmann::json(nullptr)},
            {"Group", p.group.has_value() ? nlohmann::json(*p.group) : nlohmann::json(nullptr)},
            {"DisableQueryHistory", p.disable_query_history},
        };
        profiles_json.push_back(std::move(entry));
    }

    nlohmann::json root = {
        {"Version", 1},
        {"Profiles", std::move(profiles_json)},
        {"Groups", doc.groups},
    };
    return atomic_write_utf8(path, root.dump(2));
}

} // namespace

std::optional<std::array<uint8_t, 16>> parse_guid_bytes(std::string_view uuid_str) noexcept {
    std::array<uint8_t, 16> raw{};
    size_t byte_idx = 0;
    int high = -1;

    for (const char ch : uuid_str) {
        if (ch == '-' || ch == '{' || ch == '}') {
            if (high != -1) {
                return std::nullopt;
            }
            continue;
        }
        const int nib = hex_nibble(ch);
        if (nib < 0) {
            return std::nullopt;
        }
        if (high < 0) {
            high = nib;
        } else {
            if (byte_idx >= 16) {
                return std::nullopt;
            }
            raw[byte_idx++] = static_cast<uint8_t>((high << 4) | nib);
            high = -1;
        }
    }
    if (byte_idx != 16 || high != -1) {
        return std::nullopt;
    }

    // Binary layout stores Data1 (4B), Data2 (2B), Data3 (2B) in little-endian, Data4 (8B) in big-endian.
    std::array<uint8_t, 16> guid_bytes{};
    guid_bytes[0] = raw[3];
    guid_bytes[1] = raw[2];
    guid_bytes[2] = raw[1];
    guid_bytes[3] = raw[0];
    guid_bytes[4] = raw[5];
    guid_bytes[5] = raw[4];
    guid_bytes[6] = raw[7];
    guid_bytes[7] = raw[6];
    for (size_t i = 8; i < 16; ++i) {
        guid_bytes[i] = raw[i];
    }
    return guid_bytes;
}

void secure_wipe(std::string& buffer) noexcept {
    volatile char* ptr = buffer.data();
    for (size_t i = 0; i < buffer.size(); ++i) {
        ptr[i] = '\0';
    }
    buffer.clear();
}

void secure_wipe(std::vector<uint8_t>& buffer) noexcept {
    volatile uint8_t* ptr = buffer.data();
    for (size_t i = 0; i < buffer.size(); ++i) {
        ptr[i] = 0;
    }
    buffer.clear();
}

std::string base64_encode(std::span<const uint8_t> bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i < bytes.size()) {
        const uint32_t b0 = bytes[i++];
        const bool has1 = (i < bytes.size());
        const uint32_t b1 = has1 ? bytes[i++] : 0;
        const bool has2 = (i < bytes.size());
        const uint32_t b2 = has2 ? bytes[i++] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        out.push_back(kAlphabet[(triple >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3Fu]);
        out.push_back(has1 ? kAlphabet[(triple >> 6) & 0x3Fu] : '=');
        out.push_back(has2 ? kAlphabet[triple & 0x3Fu] : '=');
    }
    return out;
}

std::optional<std::vector<uint8_t>> base64_decode(std::string_view encoded) {
    if (encoded.empty()) {
        return std::vector<uint8_t>{};
    }

    auto decode_char = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<uint8_t> out;
    out.reserve((encoded.size() / 4) * 3);

    uint32_t buf = 0;
    int bits = 0;
    int padding = 0;
    for (const char ch : encoded) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            continue;
        }
        if (ch == '=') {
            ++padding;
            continue;
        }
        if (padding > 0) {
            return std::nullopt;
        }
        const int val = decode_char(static_cast<unsigned char>(ch));
        if (val < 0) {
            return std::nullopt;
        }
        buf = (buf << 6) | static_cast<uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFFu));
        }
    }
    if (padding > 2) {
        return std::nullopt;
    }
    return out;
}

std::string validate_server_group(std::string_view path) {
    if (path.size() > 1024) {
        return "Group names must be 1 to 128 characters, without '/' or leading and trailing spaces.";
    }
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t slash = path.find('/', pos);
        const std::string_view seg = (slash == std::string_view::npos)
            ? path.substr(pos)
            : path.substr(pos, slash - pos);
        if (seg.empty() || seg.size() > 128 || trim_ascii(seg).size() != seg.size()) {
            return "Group names must be 1 to 128 characters, without '/' or leading and trailing spaces.";
        }
        for (const char ch : seg) {
            if (std::iscntrl(static_cast<unsigned char>(ch))) {
                return "Group names must be 1 to 128 characters, without '/' or leading and trailing spaces.";
            }
        }
        if (slash == std::string_view::npos) {
            break;
        }
        pos = slash + 1;
    }
    return {};
}

std::string ConnectionProfile::validate() const {
    if (!parse_guid_bytes(id).has_value()
        || trim_ascii(name).empty()
        || trim_ascii(database).empty()) {
        return "Enter a profile name, server, database, user and port from 1 to 65535.";
    }
    if (engine != "PostgreSql" && engine != "SqlServer" && engine != "SQLite" && engine != "Sqlite") {
        return "Unsupported database engine.";
    }
    const auto eng_kind = drivers::engine_kind_from_string(engine);
    const auto& desc    = drivers::driver_ui_descriptor(eng_kind);
    if (desc.storage_kind == drivers::ConnectionStorageKind::NetworkServer) {
        if (trim_ascii(host).empty() || port < 1 || port > 65535) {
            return "Enter a profile name, server, database, user and port from 1 to 65535.";
        }
        if (auth_mode != "Integrated" && trim_ascii(username).empty()) {
            return "Enter a profile name, server, database, user and port from 1 to 65535.";
        }
    } else {
        if (port < 0 || port > 65535) {
            return "Enter a valid port.";
        }
    }
    if (auth_mode != "Password" && auth_mode != "Integrated") {
        return "Unsupported authentication mode.";
    }
    if (ssl_mode != "VerifyFull" && ssl_mode != "Require" && ssl_mode != "Prefer" && ssl_mode != "Disable") {
        return "Unsupported profile SSL mode.";
    }
    if (group.has_value() && !group->empty()) {
        if (const std::string err = validate_server_group(*group); !err.empty()) {
            return err;
        }
    }
    return {};
}

std::optional<std::string> ConnectionProfile::reveal_password() const {
    if (!protected_password.has_value() || protected_password->empty()) {
        return std::string{};
    }
    return unprotect_secret(*protected_password, id);
}

void ConnectionProfile::set_password(std::string_view password, bool remember) {
    if (!remember) {
        protected_password = std::nullopt;
        return;
    }
    const std::string encoded = protect_secret(password, id);
    if (encoded.empty() && !password.empty()) {
        protected_password = std::nullopt;
    } else {
        protected_password = encoded;
    }
}

std::vector<ConnectionProfile> load_connection_profiles(
    const std::filesystem::path& path,
    std::string* error_out) {
    StoredDocument doc;
    if (!read_document(path, doc, error_out)) {
        return {};
    }
    return doc.profiles;
}

std::vector<std::string> load_connection_profile_groups(
    const std::filesystem::path& path,
    std::string* error_out) {
    StoredDocument doc;
    if (!read_document(path, doc, error_out)) {
        return {};
    }
    std::vector<std::string> groups = doc.groups;
    for (const auto& p : doc.profiles) {
        if (p.group.has_value() && !p.group->empty()) {
            std::string cur = *p.group;
            while (!cur.empty()) {
                groups.push_back(cur);
                const size_t slash = cur.rfind('/');
                if (slash == std::string::npos) {
                    break;
                }
                cur.resize(slash);
            }
        }
    }
    std::sort(groups.begin(), groups.end(), [](const std::string& a, const std::string& b) {
        return to_lower_ascii(a) < to_lower_ascii(b);
    });
    groups.erase(std::unique(groups.begin(), groups.end(), [](const std::string& a, const std::string& b) {
        return to_lower_ascii(a) == to_lower_ascii(b);
    }), groups.end());
    return groups;
}

bool save_connection_profile(
    const std::filesystem::path& path,
    const ConnectionProfile& profile,
    std::string* error_out) {
    if (const std::string err = profile.validate(); !err.empty()) {
        if (error_out != nullptr) {
            *error_out = err;
        }
        return false;
    }
    StoredDocument doc;
    if (!read_document(path, doc, error_out)) {
        return false;
    }
    const std::string target_id = to_lower_ascii(profile.id);
    std::erase_if(doc.profiles, [&](const ConnectionProfile& p) {
        return to_lower_ascii(p.id) == target_id;
    });
    ConnectionProfile normalized = profile;
    normalized.id = target_id;
    doc.profiles.push_back(std::move(normalized));
    if (!write_document(path, std::move(doc))) {
        if (error_out != nullptr) {
            *error_out = "Could not write the connection profiles file.";
        }
        return false;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool remove_connection_profile(
    const std::filesystem::path& path,
    std::string_view profile_id,
    std::string* error_out) {
    StoredDocument doc;
    if (!read_document(path, doc, error_out)) {
        return false;
    }
    const std::string target_id = to_lower_ascii(profile_id);
    std::erase_if(doc.profiles, [&](const ConnectionProfile& p) {
        return to_lower_ascii(p.id) == target_id;
    });
    if (!write_document(path, std::move(doc))) {
        if (error_out != nullptr) {
            *error_out = "Could not write the connection profiles file.";
        }
        return false;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool add_connection_profile_group(
    const std::filesystem::path& path,
    std::string_view group_path,
    std::string* error_out) {
    if (const std::string err = validate_server_group(group_path); !err.empty()) {
        if (error_out != nullptr) {
            *error_out = err;
        }
        return false;
    }
    StoredDocument doc;
    if (!read_document(path, doc, error_out)) {
        return false;
    }
    const std::string target_ci = to_lower_ascii(group_path);
    const bool exists = std::any_of(doc.groups.begin(), doc.groups.end(), [&](const std::string& g) {
        return to_lower_ascii(g) == target_ci;
    });
    if (!exists) {
        doc.groups.emplace_back(group_path);
    }
    if (!write_document(path, std::move(doc))) {
        if (error_out != nullptr) {
            *error_out = "Could not write the connection profiles file.";
        }
        return false;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool rename_connection_profile_group(
    const std::filesystem::path& path,
    std::string_view old_path,
    std::string_view new_path,
    std::string* error_out) {
    if (const std::string err = validate_server_group(new_path); !err.empty()) {
        if (error_out != nullptr) {
            *error_out = err;
        }
        return false;
    }
    StoredDocument doc;
    if (!read_document(path, doc, error_out)) {
        return false;
    }
    const std::string old_ci = to_lower_ascii(old_path);
    const std::string old_prefix_ci = old_ci + "/";

    auto rewrite = [&](std::string& g) {
        const std::string g_ci = to_lower_ascii(g);
        if (g_ci == old_ci) {
            g = std::string(new_path);
        } else if (g_ci.starts_with(old_prefix_ci)) {
            g = std::string(new_path) + g.substr(old_path.size());
        }
    };

    for (std::string& g : doc.groups) {
        rewrite(g);
    }
    for (ConnectionProfile& p : doc.profiles) {
        if (p.group.has_value() && !p.group->empty()) {
            rewrite(*p.group);
        }
    }
    if (!write_document(path, std::move(doc))) {
        if (error_out != nullptr) {
            *error_out = "Could not write the connection profiles file.";
        }
        return false;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool remove_connection_profile_group(
    const std::filesystem::path& path,
    std::string_view group_path,
    std::string* error_out) {
    StoredDocument doc;
    if (!read_document(path, doc, error_out)) {
        return false;
    }
    const std::string target_ci = to_lower_ascii(group_path);
    const std::string prefix_ci = target_ci + "/";

    auto matches = [&](std::string_view g) {
        const std::string g_ci = to_lower_ascii(g);
        return g_ci == target_ci || g_ci.starts_with(prefix_ci);
    };

    std::erase_if(doc.groups, [&](const std::string& g) { return matches(g); });
    for (ConnectionProfile& p : doc.profiles) {
        if (p.group.has_value() && matches(*p.group)) {
            p.group.reset();
        }
    }
    if (!write_document(path, std::move(doc))) {
        if (error_out != nullptr) {
            *error_out = "Could not write the connection profiles file.";
        }
        return false;
    }
    if (error_out != nullptr) {
        error_out->clear();
    }
    return true;
}

bool is_query_history_disabled_for_connection(
    const std::filesystem::path& path,
    std::string_view engine,
    std::string_view host,
    int port,
    std::string_view database) {
    const auto profiles = load_connection_profiles(path);
    const std::string eng_ci  = to_lower_ascii(engine);
    const std::string host_ci = to_lower_ascii(host);
    const std::string db_ci   = to_lower_ascii(database);
    for (const ConnectionProfile& p : profiles) {
        if (!p.disable_query_history) {
            continue;
        }
        const std::string p_eng_ci = to_lower_ascii(p.engine);
        if (p_eng_ci != eng_ci) {
            continue;
        }
        if (p_eng_ci == "sqlite") {
            if (db_ci.empty() || to_lower_ascii(p.database) == db_ci) {
                return true;
            }
        } else if (to_lower_ascii(p.host) == host_ci && p.port == port) {
            return true;
        }
    }
    return false;
}

ConnectionDialogPreferences load_connection_dialog_preferences(const std::filesystem::path& path) {
    const auto text = read_text_file(path);
    if (!text.has_value()) {
        return ConnectionDialogPreferences{};
    }
    const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return ConnectionDialogPreferences{};
    }
    if (read_int_field(j, "Version", "version", -1) != ConnectionDialogPreferences::kCurrentVersion) {
        return ConnectionDialogPreferences{};
    }

    ConnectionDialogPreferences prefs;
    prefs.last_profile = read_opt_str_field(j, "LastProfile", "last_profile");
    if (const nlohmann::json* w = find_field(j, "Width", "width"); w != nullptr && w->is_number()) {
        prefs.width = w->get<double>();
    }
    if (const nlohmann::json* h = find_field(j, "Height", "height"); h != nullptr && h->is_number()) {
        prefs.height = h->get<double>();
    }
    return prefs;
}

bool save_connection_dialog_preferences(
    const std::filesystem::path& path,
    const ConnectionDialogPreferences& prefs) {
    nlohmann::json j = {
        {"Version", ConnectionDialogPreferences::kCurrentVersion},
        {"LastProfile", prefs.last_profile.has_value() ? nlohmann::json(*prefs.last_profile) : nlohmann::json(nullptr)},
        {"Width", prefs.width.has_value() ? nlohmann::json(*prefs.width) : nlohmann::json(nullptr)},
        {"Height", prefs.height.has_value() ? nlohmann::json(*prefs.height) : nlohmann::json(nullptr)},
    };
    return atomic_write_utf8(path, j.dump(2));
}

} // namespace capybaradb::core
