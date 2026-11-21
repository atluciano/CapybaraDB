#include "core/config/state_paths.hpp"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shlobj.h>
#include <windows.h>
#endif

namespace capybaradb::core {
namespace {

std::string read_env_utf8(const char* name) {
#ifdef _WIN32
    wchar_t wname[128]{};
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, static_cast<int>(std::size(wname)));
    if (wlen <= 0) {
        return {};
    }
    const DWORD needed = GetEnvironmentVariableW(wname, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring buf(needed, L'\0');
    const DWORD written = GetEnvironmentVariableW(wname, buf.data(), needed);
    if (written == 0) {
        return {};
    }
    buf.resize(written);
    const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(utf8_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), out.data(), utf8_len, nullptr, nullptr);
    return out;
#else
    if (const char* val = std::getenv(name); val != nullptr && *val != '\0') {
        return std::string{val};
    }
    return {};
#endif
}

std::filesystem::path path_from_utf8(const std::string& utf8) {
#ifdef _WIN32
    if (utf8.empty()) {
        return {};
    }
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) {
        return {};
    }
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), wlen);
    return std::filesystem::path{std::move(w)};
#else
    return std::filesystem::path{utf8};
#endif
}

} // namespace

std::filesystem::path state_directory() {
    if (const std::string override_dir = read_env_utf8("CAPYBARADB_STATE_DIRECTORY"); !override_dir.empty()) {
        return path_from_utf8(override_dir);
    }

#ifdef _WIN32
    PWSTR local_app_data = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local_app_data)) && local_app_data != nullptr) {
        std::filesystem::path base{local_app_data};
        CoTaskMemFree(local_app_data);
        return base / "CapybaraDB";
    }
    if (const std::string fallback = read_env_utf8("LOCALAPPDATA"); !fallback.empty()) {
        return path_from_utf8(fallback) / "CapybaraDB";
    }
    if (const std::string user_profile = read_env_utf8("USERPROFILE"); !user_profile.empty()) {
        return path_from_utf8(user_profile) / "AppData" / "Local" / "CapybaraDB";
    }
    return std::filesystem::current_path() / ".capybaradb";
#else
    if (const std::string xdg = read_env_utf8("XDG_CONFIG_HOME"); !xdg.empty()) {
        return path_from_utf8(xdg) / "capybaradb";
    }
    if (const std::string home = read_env_utf8("HOME"); !home.empty()) {
        return path_from_utf8(home) / ".config" / "capybaradb";
    }
    return std::filesystem::current_path() / ".capybaradb";
#endif
}

std::filesystem::path environment_options_path() {
    return state_directory() / "environment.json";
}

std::filesystem::path explorer_toolbar_prefs_path() {
    return state_directory() / "explorer-toolbar.json";
}

std::filesystem::path dock_layout_path() {
    return state_directory() / "layout.bin";
}

std::filesystem::path connections_path() {
    return state_directory() / "connections.json";
}

std::filesystem::path connect_dialog_prefs_path() {
    return state_directory() / "connect-dialog.json";
}

std::filesystem::path query_history_path() {
    return state_directory() / "query-history.dat";
}

std::filesystem::path recovery_directory() {
    return state_directory() / "recovery";
}

} // namespace capybaradb::core
