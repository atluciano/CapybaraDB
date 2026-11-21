#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "drivers/common/driver_plugin_abi.hpp"
#include "drivers/common/driver_registry.hpp"
#include "core/config/utf_convert.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace capybaradb::drivers {

namespace {

std::string truncate_utf8_4096(std::string_view value) {
    constexpr size_t kLimit = 4096;
    if (value.size() <= kLimit) {
        return std::string(value);
    }
    size_t cut = kLimit;
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    std::string out(value.substr(0, cut));
    out += " [truncated]";
    return out;
}

std::string escape_odbc_value(std::string_view val) {
    bool needs_braces = val.empty();
    for (const char ch : val) {
        if (ch == ';' || ch == '{' || ch == '}' || std::isspace(static_cast<unsigned char>(ch))) {
            needs_braces = true;
            break;
        }
    }
    if (!needs_braces) {
        return std::string(val);
    }
    std::string out;
    out.reserve(val.size() + 4);
    out.push_back('{');
    for (const char ch : val) {
        if (ch == '}') {
            out.push_back('}');
        }
        out.push_back(ch);
    }
    out.push_back('}');
    return out;
}

constexpr size_t engine_index(EngineKind engine) noexcept {
    switch (engine) {
    case EngineKind::PostgreSql: return 0;
    case EngineKind::SqlServer:  return 1;
    case EngineKind::Sqlite:     return 2;
    }
    return 0;
}

const char* plugin_base_name(EngineKind engine) noexcept {
    switch (engine) {
    case EngineKind::PostgreSql: return "capybaradb_driver_postgres";
    case EngineKind::SqlServer:  return "capybaradb_driver_mssql";
    case EngineKind::Sqlite:     return "capybaradb_driver_sqlite";
    }
    return "capybaradb_driver_postgres";
}

std::filesystem::path executable_directory() {
#ifdef _WIN32
    std::array<wchar_t, 4096> buf{};
    const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len > 0 && len < buf.size()) {
        return std::filesystem::path(std::wstring_view(buf.data(), len)).parent_path();
    }
#else
    std::array<char, 4096> buf{};
    const ssize_t len = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (len > 0) {
        buf[static_cast<size_t>(len)] = '\0';
        return std::filesystem::path(buf.data()).parent_path();
    }
#endif
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

struct DriverModule final {
#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void*   handle = nullptr;
#endif
    EngineKind               engine            = EngineKind::PostgreSql;
    FnDriverAbiVersion       fn_abi_version    = nullptr;
    FnCreateDriver           fn_create_driver  = nullptr;
    FnDestroyDriver          fn_destroy_driver = nullptr;
    FnCreateCatalogProvider  fn_create_cat     = nullptr;
    FnDestroyCatalogProvider fn_destroy_cat    = nullptr;
    FnShutdownPlugin         fn_shutdown       = nullptr;
    bool                     can_unload        = true;

    DriverModule() = default;
    DriverModule(const DriverModule&) = delete;
    DriverModule& operator=(const DriverModule&) = delete;

    void unload() noexcept {
        if (handle != nullptr) {
            if (fn_shutdown != nullptr) {
                fn_shutdown();
            }
#ifdef _WIN32
            if (can_unload) {
                (void)FreeLibrary(handle);
            }
#else
            if (can_unload) {
                (void)::dlclose(handle);
            }
#endif
            handle            = nullptr;
            fn_abi_version    = nullptr;
            fn_create_driver  = nullptr;
            fn_destroy_driver = nullptr;
            fn_create_cat     = nullptr;
            fn_destroy_cat    = nullptr;
            fn_shutdown       = nullptr;
        }
    }

    ~DriverModule() noexcept {
        unload();
    }
};

struct ModuleSlot {
    DriverModule                          module;
    std::atomic<size_t>                   active_instances{0};
    std::chrono::steady_clock::time_point last_released = std::chrono::steady_clock::now();
    bool                                  can_unload    = true;
};

struct RegistryState {
    std::mutex                  mu;
    std::array<ModuleSlot, 3>   slots{};
};

RegistryState& registry_state() {
    static RegistryState state;
    return state;
}

template <typename Fn>
Fn load_symbol(
#ifdef _WIN32
    HMODULE handle,
#else
    void* handle,
#endif
    const char* name) noexcept {
#ifdef _WIN32
    return reinterpret_cast<Fn>(GetProcAddress(handle, name));
#else
    return reinterpret_cast<Fn>(::dlsym(handle, name));
#endif
}

ModuleSlot* acquire_module_slot(EngineKind engine) {
    auto& st = registry_state();
    std::lock_guard<std::mutex> lock(st.mu);
    ModuleSlot& slot = st.slots[engine_index(engine)];
    if (slot.module.handle != nullptr) {
        slot.active_instances.fetch_add(1, std::memory_order_acq_rel);
        return &slot;
    }

    const std::filesystem::path exe_dir = executable_directory();
    const std::string base = plugin_base_name(engine);

#ifdef _WIN32
    const std::array<std::filesystem::path, 2> candidates = {
        exe_dir / (base + ".dll"),
        std::filesystem::path(base + ".dll"),
    };
    HMODULE h = nullptr;
    for (const auto& p : candidates) {
        h = LoadLibraryW(p.c_str());
        if (h != nullptr) {
            break;
        }
    }
#else
    const std::array<std::filesystem::path, 5> candidates = {
        exe_dir / ("lib" + base + ".so"),
        exe_dir / (base + ".so"),
        exe_dir.parent_path() / "lib" / ("lib" + base + ".so"),
        std::filesystem::path("lib" + base + ".so"),
        std::filesystem::path(base + ".so"),
    };
    void* h = nullptr;
    for (const auto& p : candidates) {
        h = ::dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (h != nullptr) {
            break;
        }
    }
#endif

    if (h == nullptr) {
        return nullptr;
    }

    const auto fn_abi            = load_symbol<FnDriverAbiVersion>(h, "capybaradb_driver_abi_version");
    const auto fn_create_driver  = load_symbol<FnCreateDriver>(h, "capybaradb_create_driver");
    const auto fn_destroy_driver = load_symbol<FnDestroyDriver>(h, "capybaradb_destroy_driver");
    const auto fn_create_cat     = load_symbol<FnCreateCatalogProvider>(h, "capybaradb_create_catalog_provider");
    const auto fn_destroy_cat    = load_symbol<FnDestroyCatalogProvider>(h, "capybaradb_destroy_catalog_provider");
    const auto fn_shutdown       = load_symbol<FnShutdownPlugin>(h, "capybaradb_shutdown_plugin");

    if (fn_abi == nullptr ||
        fn_abi() != kDriverPluginAbiVersion ||
        fn_create_driver == nullptr ||
        fn_destroy_driver == nullptr ||
        fn_create_cat == nullptr ||
        fn_destroy_cat == nullptr) {
#ifdef _WIN32
        (void)FreeLibrary(h);
#else
        (void)::dlclose(h);
#endif
        return nullptr;
    }

    slot.module.handle            = h;
    slot.module.engine            = engine;
    slot.module.can_unload        = slot.can_unload;
    slot.module.fn_abi_version    = fn_abi;
    slot.module.fn_create_driver  = fn_create_driver;
    slot.module.fn_destroy_driver = fn_destroy_driver;
    slot.module.fn_create_cat     = fn_create_cat;
    slot.module.fn_destroy_cat    = fn_destroy_cat;
    slot.module.fn_shutdown       = fn_shutdown;
    slot.active_instances.fetch_add(1, std::memory_order_acq_rel);
    return &slot;
}

void release_module_slot(ModuleSlot* slot) noexcept {
    if (slot == nullptr) {
        return;
    }
    auto& st = registry_state();
    std::lock_guard<std::mutex> lock(st.mu);
    slot->active_instances.fetch_sub(1, std::memory_order_acq_rel);
    slot->last_released = std::chrono::steady_clock::now();
}

/// Proxy DbDriver whose vtable lives in capybaradb_core.
/// Ensures the plugin-allocated DbDriver is destroyed BEFORE active_instances is decremented.
class PluginDbDriver final : public DbDriver {
public:
    PluginDbDriver(ModuleSlot* slot, DbDriver* inner) noexcept
        : slot_(slot)
        , inner_(inner) {}

    ~PluginDbDriver() override {
        if (inner_ != nullptr && slot_ != nullptr && slot_->module.fn_destroy_driver != nullptr) {
            slot_->module.fn_destroy_driver(inner_);
            inner_ = nullptr;
        }
        release_module_slot(slot_);
        slot_ = nullptr;
    }

    std::expected<void, DbError> connect(const ConnectionConfig& cfg, core::CancelToken& cancel) override {
        return inner_->connect(cfg, cancel);
    }
    void disconnect() noexcept override {
        inner_->disconnect();
    }
    bool is_connected() const noexcept override {
        return inner_->is_connected();
    }
    SessionInfo session_info() const override {
        return inner_->session_info();
    }
    std::expected<ConnectionTestResult, DbError> test_connection(
        const ConnectionConfig& cfg,
        core::CancelToken& cancel) override {
        return inner_->test_connection(cfg, cancel);
    }
    std::expected<void, DbError> switch_database(
        std::string_view database,
        core::CancelToken& cancel) override {
        return inner_->switch_database(database, cancel);
    }
    std::expected<SessionLossRisk, DbError> inspect_loss_risk(core::CancelToken& cancel) override {
        return inner_->inspect_loss_risk(cancel);
    }
    std::expected<void, DbError> commit_transaction(core::CancelToken& cancel) override {
        return inner_->commit_transaction(cancel);
    }
    std::expected<void, DbError> execute_stream(
        std::string_view sql,
        const ExecOptions& opts,
        core::QueryExecutionResult& out,
        core::StreamPublishSink& sink,
        core::CancelToken& cancel) override {
        return inner_->execute_stream(sql, opts, out, sink, cancel);
    }
    std::expected<void, DbError> parse_only(
        std::string_view sql,
        core::CancelToken& cancel) override {
        return inner_->parse_only(sql, cancel);
    }
    std::expected<std::vector<core::RawExecutionPlan>, DbError> explain_plan(
        std::string_view sql,
        bool analyze,
        uint32_t timeout_sec,
        core::CancelToken& cancel) override {
        return inner_->explain_plan(sql, analyze, timeout_sec, cancel);
    }
    void cancel_active() noexcept override {
        inner_->cancel_active();
    }

private:
    ModuleSlot* slot_  = nullptr;
    DbDriver*   inner_ = nullptr;
};

/// Proxy CatalogProvider whose vtable lives in capybaradb_core.
/// Ensures the plugin-allocated CatalogProvider is destroyed BEFORE active_instances is decremented.
class PluginCatalogProvider final : public CatalogProvider {
public:
    PluginCatalogProvider(ModuleSlot* slot, CatalogProvider* inner) noexcept
        : slot_(slot)
        , inner_(inner) {}

    ~PluginCatalogProvider() override {
        if (inner_ != nullptr && slot_ != nullptr && slot_->module.fn_destroy_cat != nullptr) {
            slot_->module.fn_destroy_cat(inner_);
            inner_ = nullptr;
        }
        release_module_slot(slot_);
        slot_ = nullptr;
    }

    std::expected<std::vector<CatalogNodeData>, DbError> list_children(
        const ConnectionConfig& base_cfg,
        const CatalogNodeData& parent,
        core::CancelToken& cancel) override {
        return inner_->list_children(base_cfg, parent, cancel);
    }

    std::expected<std::vector<std::string>, DbError> list_databases(
        const ConnectionConfig& base_cfg,
        core::CancelToken& cancel) override {
        return inner_->list_databases(base_cfg, cancel);
    }

    std::expected<std::vector<std::string>, DbError> read_table_columns(
        const ConnectionConfig& base_cfg,
        std::string_view database,
        std::string_view schema,
        std::string_view table,
        core::CancelToken& cancel) override {
        return inner_->read_table_columns(base_cfg, database, schema, table, cancel);
    }

    std::string generate_select_top_sql(
        std::string_view schema,
        std::string_view object,
        std::span<const std::string> columns,
        uint32_t top_n) const override {
        return inner_->generate_select_top_sql(schema, object, columns, top_n);
    }

    std::expected<core::SqlCompletionCatalog, DbError> load_completion_catalog(
        const ConnectionConfig& base_cfg,
        core::CancelToken& cancel) override {
        return inner_->load_completion_catalog(base_cfg, cancel);
    }

private:
    ModuleSlot*      slot_  = nullptr;
    CatalogProvider* inner_ = nullptr;
};

} // namespace

std::string DbError::format_detailed(EngineKind engine) const {
    if (engine == EngineKind::Sqlite) {
        if (native_code != 0) {
            return "SQLite error (" + std::to_string(native_code) + "): " + truncate_utf8_4096(message);
        }
        return truncate_utf8_4096(message);
    }

    if (engine == EngineKind::SqlServer) {
        std::ostringstream out;
        if (native_code != 0 || position > 0) {
            out << "Msg " << native_code;
            if (!severity.empty()) {
                out << ", Level " << severity;
            }
            if (position > 0) {
                out << ", Line " << position;
            }
            if (!sql_state.empty()) {
                out << " [" << sql_state << "]";
            }
            out << "\n";
        } else if (!sql_state.empty()) {
            out << sql_state << ": ";
        }
        out << truncate_utf8_4096(message);
        return out.str();
    }

    std::string text;
    auto add = [&](std::string_view label, std::string_view value) {
        if (value.empty()) {
            return;
        }
        if (!text.empty()) {
            text.push_back('\n');
        }
        text.append(label);
        text.append(": ");
        text.append(truncate_utf8_4096(value));
    };

    add("SQLSTATE", sql_state);
    add("Severity", severity);
    add("Message", message);
    if (position > 0) {
        add("Server character position", std::to_string(position));
    }
    add("Hint", hint);
    add("Schema", schema);
    add("Table", table);
    add("Column", column);
    add("Data type", data_type);
    add("Constraint", constraint);
    return text.empty() ? message : text;
}

std::unique_ptr<DbDriver> create_db_driver(EngineKind engine) {
    ModuleSlot* slot = acquire_module_slot(engine);
    if (slot == nullptr || slot->module.fn_create_driver == nullptr) {
        release_module_slot(slot);
        return nullptr;
    }
    DbDriver* raw = slot->module.fn_create_driver();
    if (raw == nullptr) {
        release_module_slot(slot);
        return nullptr;
    }
    return std::make_unique<PluginDbDriver>(slot, raw);
}

std::unique_ptr<CatalogProvider> create_catalog_provider(EngineKind engine) {
    ModuleSlot* slot = acquire_module_slot(engine);
    if (slot == nullptr || slot->module.fn_create_cat == nullptr) {
        release_module_slot(slot);
        return nullptr;
    }
    CatalogProvider* raw = slot->module.fn_create_cat();
    if (raw == nullptr) {
        release_module_slot(slot);
        return nullptr;
    }
    return std::make_unique<PluginCatalogProvider>(slot, raw);
}

std::vector<std::string> installed_odbc_drivers() {
    std::vector<std::string> drivers;
#ifdef _WIN32
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\ODBC\\ODBCINST.INI\\ODBC Drivers",
            0,
            KEY_READ,
            &hkey) == ERROR_SUCCESS) {
        DWORD index = 0;
        std::array<wchar_t, 512> value_name{};
        std::array<BYTE, 256> data{};
        for (;;) {
            DWORD name_len = static_cast<DWORD>(value_name.size());
            DWORD data_len = static_cast<DWORD>(data.size());
            DWORD type     = 0;
            const LONG rc = RegEnumValueW(
                hkey,
                index++,
                value_name.data(),
                &name_len,
                nullptr,
                &type,
                data.data(),
                &data_len);
            if (rc != ERROR_SUCCESS) {
                break;
            }
            if (name_len > 0) {
                std::u16string u16(name_len, u'\0');
                for (DWORD i = 0; i < name_len; ++i) {
                    u16[i] = static_cast<char16_t>(value_name[i]);
                }
                drivers.push_back(core::utf16_to_utf8(u16));
            }
        }
        RegCloseKey(hkey);
    }
#else
    std::vector<std::filesystem::path> ini_paths;
    if (const char* env_ini = std::getenv("ODBCINSTINI"); env_ini != nullptr && *env_ini != '\0') {
        ini_paths.emplace_back(env_ini);
    }
    ini_paths.emplace_back("/etc/odbcinst.ini");
    ini_paths.emplace_back("/usr/local/etc/odbcinst.ini");

    for (const auto& p : ini_paths) {
        std::ifstream in(p);
        if (!in.is_open()) {
            continue;
        }
        std::string line;
        while (std::getline(in, line)) {
            const size_t first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos || line[first] != '[') {
                continue;
            }
            const size_t close = line.find(']', first + 1);
            if (close == std::string::npos || close <= first + 1) {
                continue;
            }
            std::string section = line.substr(first + 1, close - first - 1);
            if (section == "ODBC Drivers" || section == "ODBC") {
                continue;
            }
            if (std::find(drivers.begin(), drivers.end(), section) == drivers.end()) {
                drivers.push_back(std::move(section));
            }
        }
    }
#endif
    return drivers;
}

std::string detect_mssql_odbc_driver() {
    const std::vector<std::string> drivers = installed_odbc_drivers();
    for (const char* preferred : {
             "ODBC Driver 18 for SQL Server",
             "ODBC Driver 17 for SQL Server",
             "SQL Server"}) {
        if (std::find(drivers.begin(), drivers.end(), preferred) != drivers.end()) {
            return preferred;
        }
    }
    return {};
}

bool is_mssql_odbc_available() {
    return !detect_mssql_odbc_driver().empty();
}

std::string build_mssql_connection_string(
    const ConnectionConfig& cfg,
    std::string_view driver_name) {
    std::string conn;
    conn += "Driver={";
    conn.append(driver_name);
    conn += "};Server=tcp:";
    conn += cfg.host;
    conn += ",";
    conn += std::to_string(cfg.port);
    conn += ";Database=";
    conn += escape_odbc_value(cfg.database);
    conn += ";APP=";
    conn += escape_odbc_value(
        cfg.application_name.empty() ? std::string_view{"CapybaraDB"} : std::string_view{cfg.application_name});
    conn += ";";

    const bool is_driver_18 = (driver_name.find("18") != std::string_view::npos);
    switch (cfg.tls_mode) {
    case TlsMode::VerifyFull:
        conn += is_driver_18 ? "Encrypt=Strict;" : "Encrypt=Yes;TrustServerCertificate=No;";
        break;
    case TlsMode::Require:
        conn += "Encrypt=Yes;TrustServerCertificate=Yes;";
        break;
    case TlsMode::Prefer:
        conn += is_driver_18 ? "Encrypt=Optional;TrustServerCertificate=Yes;" : "Encrypt=Yes;TrustServerCertificate=Yes;";
        break;
    case TlsMode::Disable:
        conn += "Encrypt=No;";
        break;
    }

    if (cfg.auth == AuthKind::IntegratedWindowsOrKerberos) {
        conn += "Trusted_Connection=Yes;";
    } else {
        conn += "UID=";
        conn += escape_odbc_value(cfg.username);
        conn += ";PWD=";
        conn += escape_odbc_value(cfg.password);
        conn += ";";
    }
    return conn;
}

bool is_driver_module_loaded(EngineKind engine) noexcept {
    auto& st = registry_state();
    std::lock_guard<std::mutex> lock(st.mu);
    return st.slots[engine_index(engine)].module.handle != nullptr;
}

size_t active_driver_instance_count(EngineKind engine) noexcept {
    auto& st = registry_state();
    std::lock_guard<std::mutex> lock(st.mu);
    return st.slots[engine_index(engine)].active_instances.load(std::memory_order_acquire);
}

size_t unload_unused_driver_modules(std::chrono::milliseconds min_idle) noexcept {
    auto& st = registry_state();
    std::lock_guard<std::mutex> lock(st.mu);
    const auto now = std::chrono::steady_clock::now();
    size_t unloaded = 0;

    for (ModuleSlot& slot : st.slots) {
        if (slot.module.handle != nullptr &&
            slot.can_unload &&
            slot.active_instances.load(std::memory_order_acquire) == 0) {
            if (min_idle.count() <= 0 || (now - slot.last_released) >= min_idle) {
                slot.module.unload();
                ++unloaded;
            }
        }
    }
    return unloaded;
}

} // namespace capybaradb::drivers
