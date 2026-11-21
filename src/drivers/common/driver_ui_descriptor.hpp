#pragma once

#include "drivers/common/db_types.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace capybaradb::drivers {

enum class ConnectionStorageKind : uint8_t {
    NetworkServer = 0, // PostgreSQL, SQL Server, future MySQL/Oracle
    LocalFile     = 1, // SQLite, future DuckDB
};

struct DriverUiDescriptor {
    EngineKind            engine;
    std::string_view      display_name;       // "PostgreSQL", "SQL Server", "SQLite"
    std::string_view      subtitle;           // Dynamic dialog header subtitle
    ConnectionStorageKind storage_kind;
    uint16_t              default_port;       // 5432, 1433, 0
    std::string_view      default_database;   // "postgres", "master", ":memory:"
    std::string_view      default_username;   // "postgres", "sa", ""
    bool                  supports_integrated_auth;
    bool                  supports_tls;
    bool                  supports_root_cert;
    bool                  supports_in_memory;
    std::string_view      file_filter;        // "SQLite Databases (*.db *.sqlite *.sqlite3);;All Files (*.*)"
    std::string_view      default_file_ext;   // ".db"
};

inline constexpr std::array<DriverUiDescriptor, 3> kDriverUiDescriptors = {{
    DriverUiDescriptor{
        .engine                   = EngineKind::PostgreSql,
        .display_name             = "PostgreSQL",
        .subtitle                 = "Connect to a PostgreSQL server",
        .storage_kind             = ConnectionStorageKind::NetworkServer,
        .default_port             = 5432,
        .default_database         = "postgres",
        .default_username         = "postgres",
        .supports_integrated_auth = false,
        .supports_tls             = true,
        .supports_root_cert       = true,
        .supports_in_memory       = false,
        .file_filter              = {},
        .default_file_ext         = {},
    },
    DriverUiDescriptor{
        .engine                   = EngineKind::SqlServer,
        .display_name             = "SQL Server",
        .subtitle                 = "Connect to a Microsoft SQL Server instance",
        .storage_kind             = ConnectionStorageKind::NetworkServer,
        .default_port             = 1433,
        .default_database         = "master",
        .default_username         = "sa",
        .supports_integrated_auth = true,
        .supports_tls             = true,
        .supports_root_cert       = false,
        .supports_in_memory       = false,
        .file_filter              = {},
        .default_file_ext         = {},
    },
    DriverUiDescriptor{
        .engine                   = EngineKind::Sqlite,
        .display_name             = "SQLite",
        .subtitle                 = "Connect to a SQLite local database file",
        .storage_kind             = ConnectionStorageKind::LocalFile,
        .default_port             = 0,
        .default_database         = ":memory:",
        .default_username         = "",
        .supports_integrated_auth = false,
        .supports_tls             = false,
        .supports_root_cert       = false,
        .supports_in_memory       = true,
        .file_filter              = "SQLite Databases (*.db *.sqlite *.sqlite3);;All Files (*.*)",
        .default_file_ext         = ".db",
    },
}};

[[nodiscard]] constexpr std::span<const DriverUiDescriptor> all_driver_ui_descriptors() noexcept {
    return kDriverUiDescriptors;
}

[[nodiscard]] constexpr const DriverUiDescriptor& driver_ui_descriptor(EngineKind kind) noexcept {
    for (const auto& desc : kDriverUiDescriptors) {
        if (desc.engine == kind) {
            return desc;
        }
    }
    return kDriverUiDescriptors[0];
}

} // namespace capybaradb::drivers
