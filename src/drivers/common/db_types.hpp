#pragma once

#include "core/config/query_options.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace capybaradb::drivers {

enum class EngineKind : uint8_t {
    PostgreSql = 0,
    SqlServer  = 1,
    Sqlite     = 2,
};

enum class TlsMode : uint8_t {
    VerifyFull = 0,
    Require    = 1,
    Prefer     = 2,
    Disable    = 3,
};

enum class AuthKind : uint8_t {
    Password                    = 0,
    IntegratedWindowsOrKerberos = 1,
};

[[nodiscard]] constexpr std::string_view engine_kind_to_string(EngineKind kind) noexcept {
    switch (kind) {
    case EngineKind::PostgreSql: return "PostgreSql";
    case EngineKind::SqlServer:  return "SqlServer";
    case EngineKind::Sqlite:     return "SQLite";
    }
    return "PostgreSql";
}

[[nodiscard]] constexpr EngineKind engine_kind_from_string(std::string_view text) noexcept {
    if (text == "SqlServer" || text == "SQL Server") {
        return EngineKind::SqlServer;
    }
    if (text == "Sqlite" || text == "SQLite") {
        return EngineKind::Sqlite;
    }
    return EngineKind::PostgreSql;
}

[[nodiscard]] constexpr std::string_view tls_mode_to_string(TlsMode mode) noexcept {
    switch (mode) {
    case TlsMode::VerifyFull: return "VerifyFull";
    case TlsMode::Require:    return "Require";
    case TlsMode::Prefer:     return "Prefer";
    case TlsMode::Disable:    return "Disable";
    }
    return "VerifyFull";
}

[[nodiscard]] constexpr TlsMode tls_mode_from_string(std::string_view text) noexcept {
    if (text == "Require") {
        return TlsMode::Require;
    }
    if (text == "Prefer") {
        return TlsMode::Prefer;
    }
    if (text == "Disable") {
        return TlsMode::Disable;
    }
    return TlsMode::VerifyFull;
}

[[nodiscard]] constexpr std::string_view auth_kind_to_string(AuthKind auth) noexcept {
    switch (auth) {
    case AuthKind::Password:                    return "Password";
    case AuthKind::IntegratedWindowsOrKerberos: return "Integrated";
    }
    return "Password";
}

[[nodiscard]] constexpr AuthKind auth_kind_from_string(std::string_view text) noexcept {
    if (text == "Integrated" || text == "IntegratedWindowsOrKerberos") {
        return AuthKind::IntegratedWindowsOrKerberos;
    }
    return AuthKind::Password;
}

struct ConnectionConfig {
    EngineKind  engine              = EngineKind::PostgreSql;
    std::string host                = "localhost";
    uint16_t    port                = 5432;
    std::string database            = "postgres";
    std::string username            = "postgres";
    std::string password;
    AuthKind    auth                = AuthKind::Password;
    TlsMode     tls_mode            = TlsMode::VerifyFull;
    std::string root_cert_path;
    std::string application_name    = "CapybaraDB";
    uint32_t    connect_timeout_sec = 10;

    ConnectionConfig() = default;
    ConnectionConfig(const ConnectionConfig&) = default;
    ConnectionConfig& operator=(const ConnectionConfig&) = default;
    ConnectionConfig(ConnectionConfig&&) noexcept = default;
    ConnectionConfig& operator=(ConnectionConfig&&) noexcept = default;

    ~ConnectionConfig() noexcept {
        volatile char* ptr = password.data();
        for (size_t i = 0; i < password.size(); ++i) {
            ptr[i] = '\0';
        }
        password.clear();
    }

    [[nodiscard]] ConnectionConfig with_database(std::string new_db) const {
        ConnectionConfig copy = *this;
        copy.database = std::move(new_db);
        return copy;
    }

    [[nodiscard]] std::string display_server() const {
        if (engine == EngineKind::Sqlite) {
            return database.empty() ? std::string{":memory:"} : database;
        }
        return host + ":" + std::to_string(port) + " (" +
               (auth == AuthKind::IntegratedWindowsOrKerberos && username.empty()
                    ? std::string{"Integrated"}
                    : username) +
               ")";
    }
};

struct SessionInfo {
    EngineKind  engine = EngineKind::PostgreSql;
    std::string host;
    uint16_t    port = 5432;
    std::string server_version;
    std::string database;
    std::string username;
    int64_t     session_id = 0; // Backend PID (PG) or @@SPID (SQL Server)
};

struct SessionLossRisk {
    bool open_transaction    = false;
    bool aborted_transaction = false;
    bool temporary_tables    = false;

    [[nodiscard]] constexpr bool has_any_risk() const noexcept {
        return open_transaction || aborted_transaction || temporary_tables;
    }
};

struct ExecOptions {
    uint32_t                  command_timeout_sec = 0;
    uint64_t                  max_arena_bytes     = 1024ull * 1024 * 1024;
    uint32_t                  max_cell_bytes      = 65536;
    bool                      discard_results     = false;
    bool                      nocount             = false;
    bool                      include_actual_plan = false;
    core::QueryIsolationLevel isolation           = core::QueryIsolationLevel::ServerDefault;
    int32_t                   lock_timeout_ms     = -1;
};

struct DbError {
    std::string sql_state  = {};
    std::string severity   = {};
    std::string message    = {};
    std::string hint       = {};
    std::string schema     = {};
    std::string table      = {};
    std::string column     = {};
    std::string data_type  = {};
    std::string constraint = {};
    int32_t     position    = -1;
    int32_t     native_code = 0;
    bool        is_canceled = false;

    [[nodiscard]] std::string format_short() const {
        if (!sql_state.empty()) {
            return sql_state + ": " + message;
        }
        return message;
    }

    [[nodiscard]] std::string format_detailed(EngineKind engine = EngineKind::PostgreSql) const;
};

struct ConnectionTestResult {
    std::string database   = {};
    std::string user       = {};
    std::string version    = {};
    bool        tls_active = false;
};

} // namespace capybaradb::drivers
