#pragma once

#include "drivers/common/db_types.hpp"

#include <optional>
#include <string_view>

namespace capybaradb::core {

struct ParsedConnectionUri {
    drivers::ConnectionConfig config;
    bool                      has_engine   = false;
    bool                      has_host     = false;
    bool                      has_port     = false;
    bool                      has_database = false;
    bool                      has_username = false;
    bool                      has_password = false;
    bool                      has_auth     = false;
    bool                      has_tls_mode = false;
    bool                      has_timeout  = false;
    bool                      has_app_name = false;
};

/// Parses connection strings / URIs such as:
/// - postgresql://user:pass@host:5432/dbname?sslmode=require&connect_timeout=15&application_name=MyApp
/// - mssql://user:pass@host:1433/dbname
/// - sqlserver://host:1433/dbname?trusted_connection=yes
/// - sqlite:///C:/dados/app.db or sqlite://:memory: or file:meu_banco.db
/// - Server=host,1433;Database=db;User Id=sa;Password=...;Trusted_Connection=Yes;Encrypt=Strict;
/// - host=localhost port=5432 dbname=postgres user=postgres password=... sslmode=verify-full
[[nodiscard]] std::optional<ParsedConnectionUri> parse_connection_uri(std::string_view text);

} // namespace capybaradb::core
