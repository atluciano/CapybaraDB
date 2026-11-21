#include "core/config/connection_uri_parser.hpp"

#include "drivers/common/driver_ui_descriptor.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace capybaradb::core {
namespace {

std::string_view trim_sv(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

int hex_val(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string percent_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hex_val(in[i + 1]);
            const int lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

std::optional<uint16_t> parse_port_u16(std::string_view s) noexcept {
    s = trim_sv(s);
    if (s.empty()) {
        return std::nullopt;
    }
    uint32_t val = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec != std::errc{} || ptr != s.data() + s.size() || val == 0 || val > 65535) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(val);
}

std::optional<uint32_t> parse_timeout_u32(std::string_view s) noexcept {
    s = trim_sv(s);
    if (s.empty()) {
        return std::nullopt;
    }
    uint32_t val = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec != std::errc{} || ptr != s.data() + s.size() || val == 0 || val > 3600) {
        return std::nullopt;
    }
    return val;
}

drivers::TlsMode parse_tls_param(std::string_view val) noexcept {
    const std::string low = to_lower_ascii(trim_sv(val));
    if (low == "disable" || low == "false" || low == "no" || low == "0" || low == "off") {
        return drivers::TlsMode::Disable;
    }
    if (low == "prefer" || low == "allow" || low == "optional") {
        return drivers::TlsMode::Prefer;
    }
    if (low == "require" || low == "required" || low == "true" || low == "yes" || low == "1") {
        return drivers::TlsMode::Require;
    }
    if (low == "verify-full" || low == "verifyfull" || low == "verify-ca" || low == "strict") {
        return drivers::TlsMode::VerifyFull;
    }
    return drivers::TlsMode::VerifyFull;
}

bool is_truthy(std::string_view val) noexcept {
    const std::string low = to_lower_ascii(trim_sv(val));
    return low == "yes" || low == "true" || low == "sspi" || low == "1" || low == "integrated";
}

void apply_query_params(std::string_view query, ParsedConnectionUri& out) {
    size_t pos = 0;
    while (pos < query.size()) {
        const size_t amp = query.find('&', pos);
        const std::string_view pair = (amp == std::string_view::npos)
            ? query.substr(pos)
            : query.substr(pos, amp - pos);
        if (!pair.empty()) {
            const size_t eq = pair.find('=');
            if (eq != std::string_view::npos) {
                const std::string key = to_lower_ascii(percent_decode(pair.substr(0, eq)));
                const std::string val = percent_decode(pair.substr(eq + 1));
                if (key == "sslmode" || key == "ssl" || key == "encrypt") {
                    out.config.tls_mode = parse_tls_param(val);
                    out.has_tls_mode = true;
                } else if (key == "sslrootcert" || key == "root_cert") {
                    out.config.root_cert_path = val;
                } else if (key == "connect_timeout" || key == "timeout") {
                    if (auto t = parse_timeout_u32(val)) {
                        out.config.connect_timeout_sec = *t;
                        out.has_timeout = true;
                    }
                } else if (key == "application_name" || key == "app") {
                    if (!val.empty()) {
                        out.config.application_name = val;
                        out.has_app_name = true;
                    }
                } else if (key == "trusted_connection" || key == "integrated security" || key == "integrated_security") {
                    if (is_truthy(val)) {
                        out.config.auth = drivers::AuthKind::IntegratedWindowsOrKerberos;
                        out.has_auth = true;
                    }
                } else if (key == "user" || key == "username" || key == "uid") {
                    out.config.username = val;
                    out.has_username = true;
                } else if (key == "password" || key == "pwd") {
                    out.config.password = val;
                    out.has_password = true;
                } else if (key == "dbname" || key == "database") {
                    out.config.database = val;
                    out.has_database = true;
                }
            }
        }
        if (amp == std::string_view::npos) {
            break;
        }
        pos = amp + 1;
    }
}

std::optional<ParsedConnectionUri> parse_url_scheme(
    drivers::EngineKind engine,
    std::string_view rest) {
    ParsedConnectionUri out;
    const auto& desc = drivers::driver_ui_descriptor(engine);
    out.config.engine   = engine;
    out.config.port     = desc.default_port;
    out.config.database = std::string(desc.default_database);
    out.config.username = std::string(desc.default_username);
    out.has_engine      = true;

    std::string_view query;
    if (const size_t qpos = rest.find('?'); qpos != std::string_view::npos) {
        query = rest.substr(qpos + 1);
        rest  = rest.substr(0, qpos);
    }

    if (engine == drivers::EngineKind::Sqlite) {
        // sqlite:///C:/path/to.db or sqlite:///path/to.db or sqlite://:memory:
        std::string_view path_part = rest;
        if (path_part.starts_with("///")) {
            // Check Windows drive letter like ///C:/...
            if (path_part.size() >= 5 &&
                std::isalpha(static_cast<unsigned char>(path_part[3])) &&
                (path_part[4] == ':' || path_part[4] == '|')) {
                path_part.remove_prefix(3);
            } else {
                path_part.remove_prefix(2); // Preserve leading '/' on POSIX paths
            }
        } else if (path_part.starts_with("//")) {
            path_part.remove_prefix(2);
        }
        std::string decoded = percent_decode(path_part);
        if (decoded.size() >= 2 && std::isalpha(static_cast<unsigned char>(decoded[0])) && decoded[1] == '|') {
            decoded[1] = ':';
        }
        if (decoded.empty() || decoded == ":memory:" || decoded == "/:memory:") {
            decoded = ":memory:";
        }
        out.config.host     = "localhost";
        out.config.port     = 0;
        out.config.database = std::move(decoded);
        out.config.username = "sqlite";
        out.has_database    = true;
        return out;
    }

    if (!rest.starts_with("//")) {
        return std::nullopt;
    }
    rest.remove_prefix(2);

    std::string_view authority = rest;
    std::string_view path_part;
    if (const size_t slash = rest.find('/'); slash != std::string_view::npos) {
        authority = rest.substr(0, slash);
        path_part = rest.substr(slash + 1);
    }

    if (const size_t at = authority.rfind('@'); at != std::string_view::npos) {
        const std::string_view userinfo = authority.substr(0, at);
        authority = authority.substr(at + 1);
        if (const size_t colon = userinfo.find(':'); colon != std::string_view::npos) {
            out.config.username = percent_decode(userinfo.substr(0, colon));
            out.config.password = percent_decode(userinfo.substr(colon + 1));
            out.has_username = true;
            out.has_password = true;
        } else {
            out.config.username = percent_decode(userinfo);
            out.has_username = true;
        }
    }

    // Parse host[:port] or [ipv6][:port]
    if (!authority.empty()) {
        std::string_view host_sv;
        std::string_view port_sv;
        if (authority.front() == '[') {
            const size_t close_br = authority.find(']');
            if (close_br == std::string_view::npos) {
                return std::nullopt;
            }
            host_sv = authority.substr(1, close_br - 1);
            if (close_br + 1 < authority.size() && authority[close_br + 1] == ':') {
                port_sv = authority.substr(close_br + 2);
            }
        } else if (const size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
            host_sv = authority.substr(0, colon);
            port_sv = authority.substr(colon + 1);
        } else {
            host_sv = authority;
        }

        if (!host_sv.empty()) {
            out.config.host = percent_decode(host_sv);
            out.has_host = true;
        }
        if (!port_sv.empty()) {
            const auto p = parse_port_u16(port_sv);
            if (!p.has_value()) {
                return std::nullopt;
            }
            out.config.port = *p;
            out.has_port = true;
        }
    }

    if (!path_part.empty()) {
        const std::string db = percent_decode(path_part);
        if (!db.empty()) {
            out.config.database = db;
            out.has_database = true;
        }
    }

    if (!query.empty()) {
        apply_query_params(query, out);
    }

    if (!out.has_host && !out.has_database && !out.has_username) {
        return std::nullopt;
    }
    return out;
}

std::string_view unquote_value(std::string_view v) noexcept {
    v = trim_sv(v);
    if (v.size() >= 2) {
        if ((v.front() == '{' && v.back() == '}') ||
            (v.front() == '"' && v.back() == '"') ||
            (v.front() == '\'' && v.back() == '\'')) {
            return v.substr(1, v.size() - 2);
        }
    }
    return v;
}

std::optional<ParsedConnectionUri> parse_semicolon_kv(std::string_view text) {
    ParsedConnectionUri out;
    out.config.engine   = drivers::EngineKind::SqlServer;
    out.config.port     = 1433;
    out.config.database = "master";
    out.config.username = "sa";

    bool matched_any = false;
    bool saw_mssql_hint = false;
    bool saw_pg_hint    = false;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t semi = text.find(';', pos);
        // Handle brace-escaped values like Password={a;b}
        const size_t open_brace = text.find('{', pos);
        if (open_brace != std::string_view::npos && (semi == std::string_view::npos || open_brace < semi)) {
            const size_t close_brace = text.find('}', open_brace + 1);
            if (close_brace != std::string_view::npos) {
                semi = text.find(';', close_brace + 1);
            }
        }

        const std::string_view seg = trim_sv((semi == std::string_view::npos)
            ? text.substr(pos)
            : text.substr(pos, semi - pos));
        if (!seg.empty()) {
            const size_t eq = seg.find('=');
            if (eq == std::string_view::npos) {
                return std::nullopt;
            }
            const std::string key = to_lower_ascii(trim_sv(seg.substr(0, eq)));
            std::string_view val  = unquote_value(seg.substr(eq + 1));

            if (key == "server" || key == "data source" || key == "addr" || key == "address" || key == "host") {
                if (key == "server" || key == "data source") {
                    saw_mssql_hint = true;
                }
                if (val.starts_with("tcp:") || val.starts_with("TCP:")) {
                    val.remove_prefix(4);
                    saw_mssql_hint = true;
                }
                // Check host,port or host:port
                size_t sep = val.rfind(',');
                if (sep != std::string_view::npos) {
                    saw_mssql_hint = true;
                } else {
                    sep = val.rfind(':');
                }
                if (sep != std::string_view::npos) {
                    const std::string_view h = trim_sv(val.substr(0, sep));
                    const std::string_view p = trim_sv(val.substr(sep + 1));
                    if (!h.empty()) {
                        out.config.host = std::string(h);
                        out.has_host = true;
                        matched_any = true;
                    }
                    if (auto parsed_p = parse_port_u16(p)) {
                        out.config.port = *parsed_p;
                        out.has_port = true;
                    }
                } else if (!val.empty()) {
                    out.config.host = std::string(val);
                    out.has_host = true;
                    matched_any = true;
                }
            } else if (key == "port") {
                if (auto parsed_p = parse_port_u16(val)) {
                    out.config.port = *parsed_p;
                    out.has_port = true;
                    matched_any = true;
                }
            } else if (key == "database" || key == "initial catalog" || key == "dbname") {
                if (key == "initial catalog") saw_mssql_hint = true;
                if (key == "dbname") saw_pg_hint = true;
                out.config.database = std::string(val);
                out.has_database = true;
                matched_any = true;
            } else if (key == "user id" || key == "uid" || key == "user" || key == "username") {
                if (key == "user id" || key == "uid") saw_mssql_hint = true;
                out.config.username = std::string(val);
                out.has_username = true;
                matched_any = true;
            } else if (key == "password" || key == "pwd") {
                if (key == "pwd") saw_mssql_hint = true;
                out.config.password = std::string(val);
                out.has_password = true;
                matched_any = true;
            } else if (key == "trusted_connection" || key == "integrated security") {
                saw_mssql_hint = true;
                if (is_truthy(val)) {
                    out.config.auth = drivers::AuthKind::IntegratedWindowsOrKerberos;
                    out.has_auth = true;
                    matched_any = true;
                }
            } else if (key == "encrypt" || key == "sslmode" || key == "ssl mode") {
                if (key == "sslmode") saw_pg_hint = true;
                out.config.tls_mode = parse_tls_param(val);
                out.has_tls_mode = true;
                matched_any = true;
            } else if (key == "trustservercertificate") {
                saw_mssql_hint = true;
                if (is_truthy(val) && out.config.tls_mode == drivers::TlsMode::VerifyFull) {
                    out.config.tls_mode = drivers::TlsMode::Require;
                    out.has_tls_mode = true;
                }
                matched_any = true;
            } else if (key == "app" || key == "application name" || key == "application_name") {
                if (!val.empty()) {
                    out.config.application_name = std::string(val);
                    out.has_app_name = true;
                }
            } else if (key == "connect timeout" || key == "connection timeout" || key == "login timeout" || key == "timeout") {
                if (auto t = parse_timeout_u32(val)) {
                    out.config.connect_timeout_sec = *t;
                    out.has_timeout = true;
                }
            } else if (key == "driver") {
                const std::string low_drv = to_lower_ascii(val);
                if (low_drv.find("postgres") != std::string::npos) {
                    saw_pg_hint = true;
                } else if (low_drv.find("sql server") != std::string::npos) {
                    saw_mssql_hint = true;
                }
                matched_any = true;
            }
        }

        if (semi == std::string_view::npos) {
            break;
        }
        pos = semi + 1;
    }

    if (!matched_any) {
        return std::nullopt;
    }

    if (saw_pg_hint && !saw_mssql_hint) {
        out.config.engine = drivers::EngineKind::PostgreSql;
        if (!out.has_port) out.config.port = 5432;
        if (!out.has_database) out.config.database = "postgres";
        if (!out.has_username) out.config.username = "postgres";
        out.has_engine = true;
    } else if (saw_mssql_hint) {
        out.config.engine = drivers::EngineKind::SqlServer;
        out.has_engine = true;
    }
    return out;
}

std::optional<ParsedConnectionUri> parse_libpq_kv(std::string_view text) {
    ParsedConnectionUri out;
    out.config.engine   = drivers::EngineKind::PostgreSql;
    out.config.port     = 5432;
    out.config.database = "postgres";
    out.config.username = "postgres";
    out.has_engine      = true;

    bool matched_any = false;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i >= text.size()) break;

        const size_t eq = text.find('=', i);
        if (eq == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string key = to_lower_ascii(trim_sv(text.substr(i, eq - i)));
        i = eq + 1;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }

        std::string val;
        if (i < text.size() && (text[i] == '\'' || text[i] == '"')) {
            const char quote = text[i++];
            while (i < text.size() && text[i] != quote) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    ++i;
                }
                val.push_back(text[i++]);
            }
            if (i < text.size() && text[i] == quote) {
                ++i;
            }
        } else {
            const size_t start = i;
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
                ++i;
            }
            val = std::string(text.substr(start, i - start));
        }

        if (key == "host" || key == "hostaddr") {
            out.config.host = val;
            out.has_host = true;
            matched_any = true;
        } else if (key == "port") {
            if (auto p = parse_port_u16(val)) {
                out.config.port = *p;
                out.has_port = true;
                matched_any = true;
            }
        } else if (key == "dbname" || key == "database") {
            out.config.database = val;
            out.has_database = true;
            matched_any = true;
        } else if (key == "user" || key == "username") {
            out.config.username = val;
            out.has_username = true;
            matched_any = true;
        } else if (key == "password") {
            out.config.password = val;
            out.has_password = true;
            matched_any = true;
        } else if (key == "sslmode") {
            out.config.tls_mode = parse_tls_param(val);
            out.has_tls_mode = true;
            matched_any = true;
        } else if (key == "sslrootcert") {
            out.config.root_cert_path = val;
            matched_any = true;
        } else if (key == "connect_timeout") {
            if (auto t = parse_timeout_u32(val)) {
                out.config.connect_timeout_sec = *t;
                out.has_timeout = true;
                matched_any = true;
            }
        } else if (key == "application_name") {
            if (!val.empty()) {
                out.config.application_name = val;
                out.has_app_name = true;
                matched_any = true;
            }
        } else {
            return std::nullopt;
        }
    }

    if (!matched_any) {
        return std::nullopt;
    }
    return out;
}

} // namespace

std::optional<ParsedConnectionUri> parse_connection_uri(std::string_view text) {
    text = trim_sv(text);
    if (text.empty()) {
        return std::nullopt;
    }

    // 1. Check URL schemes
    if (const size_t colon = text.find(':'); colon != std::string_view::npos) {
        const std::string scheme = to_lower_ascii(text.substr(0, colon));
        const std::string_view rest = text.substr(colon + 1);

        if (scheme == "postgres" || scheme == "postgresql") {
            return parse_url_scheme(drivers::EngineKind::PostgreSql, rest);
        }
        if (scheme == "mssql" || scheme == "sqlserver") {
            return parse_url_scheme(drivers::EngineKind::SqlServer, rest);
        }
        if (scheme == "sqlite" || scheme == "sqlite3") {
            return parse_url_scheme(drivers::EngineKind::Sqlite, rest);
        }
        if (scheme == "file" && !rest.empty()) {
            // file:meu_banco.db or file:///C:/dados/app.db
            return parse_url_scheme(drivers::EngineKind::Sqlite, rest);
        }
    }

    // 2. Check semicolon-delimited connection string (Server=...;Database=...;)
    if (text.find(';') != std::string_view::npos && text.find('=') != std::string_view::npos) {
        if (auto res = parse_semicolon_kv(text)) {
            return res;
        }
    }

    // 3. Check libpq space-delimited key=value format (host=localhost port=5432 dbname=...)
    if (text.find('=') != std::string_view::npos) {
        if (auto res = parse_libpq_kv(text)) {
            return res;
        }
        if (auto res = parse_semicolon_kv(text)) {
            return res;
        }
    }

    return std::nullopt;
}

} // namespace capybaradb::core
