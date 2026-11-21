#pragma once

#include "drivers/common/catalog_provider.hpp"
#include "drivers/common/db_driver.hpp"
#include "drivers/common/db_types.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace capybaradb::drivers {

[[nodiscard]] std::unique_ptr<DbDriver> create_db_driver(EngineKind engine);
[[nodiscard]] std::unique_ptr<CatalogProvider> create_catalog_provider(EngineKind engine);

/// Enumerates installed ODBC drivers directly from the OS registry / odbcinst.ini
/// WITHOUT loading capybaradb_driver_mssql or odbc32.dll.
[[nodiscard]] std::vector<std::string> installed_odbc_drivers();
[[nodiscard]] std::string detect_mssql_odbc_driver();
[[nodiscard]] bool is_mssql_odbc_available();

/// Pure string builder for SQL Server ODBC connection strings (does not load ODBC).
[[nodiscard]] std::string build_mssql_connection_string(
    const ConnectionConfig& cfg,
    std::string_view driver_name);

/// Dynamic plugin lifecycle inspection & refcounted unload management.
[[nodiscard]] bool is_driver_module_loaded(EngineKind engine) noexcept;
[[nodiscard]] size_t active_driver_instance_count(EngineKind engine) noexcept;

/// Unloads any driver plugin module that has 0 active DbDriver/CatalogProvider instances
/// and has been idle for at least min_idle. Returns the number of modules unloaded.
size_t unload_unused_driver_modules(
    std::chrono::milliseconds min_idle = std::chrono::milliseconds{0}) noexcept;

} // namespace capybaradb::drivers
