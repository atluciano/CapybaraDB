#pragma once

#include "core/result/result_set_storage.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace capybaradb::drivers {

enum class PgLogicalKind : uint8_t {
    Unknown = 0,
    Bool,
    Int,
    Float,
    Numeric,
    Text,
    Json,
    Xml,
    Binary,
    Date,
    Time,
    Timestamp,
    Uuid,
    Array,
};

struct PgTypeEntry {
    uint32_t         oid;
    std::string_view name;
    PgLogicalKind    kind;
    bool             may_hold_json;
};

/// Comprehensive built-in PostgreSQL OID table covering scalars, arrays, ranges, multiranges,
/// geometric, network, bitstring, text-search, money, interval, and bytea types.
inline constexpr auto kPgBuiltinTypeMap = std::to_array<PgTypeEntry>({
    // Core scalars
    {16u,   "bool",            PgLogicalKind::Bool,      false},
    {17u,   "bytea",           PgLogicalKind::Binary,    false},
    {18u,   "char",            PgLogicalKind::Text,      false},
    {19u,   "name",            PgLogicalKind::Text,      false},
    {20u,   "int8",            PgLogicalKind::Int,       false},
    {21u,   "int2",            PgLogicalKind::Int,       false},
    {23u,   "int4",            PgLogicalKind::Int,       false},
    {24u,   "regproc",         PgLogicalKind::Text,      false},
    {25u,   "text",            PgLogicalKind::Text,      true},
    {26u,   "oid",             PgLogicalKind::Int,       false},
    {114u,  "json",            PgLogicalKind::Json,      true},
    {142u,  "xml",             PgLogicalKind::Xml,       false},
    {700u,  "float4",          PgLogicalKind::Float,     false},
    {701u,  "float8",          PgLogicalKind::Float,     false},
    {790u,  "money",           PgLogicalKind::Numeric,   false},
    {1042u, "bpchar",          PgLogicalKind::Text,      true},
    {1043u, "varchar",         PgLogicalKind::Text,      true},
    {1082u, "date",            PgLogicalKind::Date,      false},
    {1083u, "time",            PgLogicalKind::Time,      false},
    {1114u, "timestamp",       PgLogicalKind::Timestamp, false},
    {1184u, "timestamptz",     PgLogicalKind::Timestamp, false},
    {1186u, "interval",        PgLogicalKind::Text,      false},
    {1266u, "timetz",          PgLogicalKind::Time,      false},
    {1560u, "bit",             PgLogicalKind::Text,      false},
    {1562u, "varbit",          PgLogicalKind::Text,      false},
    {1700u, "numeric",         PgLogicalKind::Numeric,   false},
    {2950u, "uuid",            PgLogicalKind::Uuid,      false},
    {3614u, "tsvector",        PgLogicalKind::Text,      false},
    {3615u, "tsquery",         PgLogicalKind::Text,      false},
    {3802u, "jsonb",           PgLogicalKind::Json,      true},

    // Geometric types
    {600u,  "point",           PgLogicalKind::Text,      false},
    {601u,  "lseg",            PgLogicalKind::Text,      false},
    {602u,  "path",            PgLogicalKind::Text,      false},
    {603u,  "box",             PgLogicalKind::Text,      false},
    {604u,  "polygon",         PgLogicalKind::Text,      false},
    {628u,  "line",            PgLogicalKind::Text,      false},
    {718u,  "circle",          PgLogicalKind::Text,      false},

    // Network types
    {650u,  "cidr",            PgLogicalKind::Text,      false},
    {774u,  "macaddr8",        PgLogicalKind::Text,      false},
    {829u,  "macaddr",         PgLogicalKind::Text,      false},
    {869u,  "inet",            PgLogicalKind::Text,      false},

    // Built-in range and multirange types (may_hold_json = false so '[1,10)' is never treated as JSON)
    {3904u, "int4range",       PgLogicalKind::Text,      false},
    {3906u, "numrange",        PgLogicalKind::Text,      false},
    {3908u, "tsrange",         PgLogicalKind::Text,      false},
    {3910u, "tstzrange",       PgLogicalKind::Text,      false},
    {3912u, "daterange",       PgLogicalKind::Text,      false},
    {3926u, "int8range",       PgLogicalKind::Text,      false},
    {4451u, "int4multirange",  PgLogicalKind::Text,      false},
    {4532u, "nummultirange",   PgLogicalKind::Text,      false},
    {4533u, "tsmultirange",    PgLogicalKind::Text,      false},
    {4534u, "tstzmultirange",  PgLogicalKind::Text,      false},
    {4535u, "datemultirange",  PgLogicalKind::Text,      false},
    {4536u, "int8multirange",  PgLogicalKind::Text,      false},

    // Built-in array types
    {199u,  "json[]",          PgLogicalKind::Array,     false},
    {651u,  "cidr[]",          PgLogicalKind::Array,     false},
    {791u,  "money[]",         PgLogicalKind::Array,     false},
    {1000u, "bool[]",          PgLogicalKind::Array,     false},
    {1001u, "bytea[]",         PgLogicalKind::Array,     false},
    {1002u, "char[]",          PgLogicalKind::Array,     false},
    {1003u, "name[]",          PgLogicalKind::Array,     false},
    {1005u, "int2[]",          PgLogicalKind::Array,     false},
    {1007u, "int4[]",          PgLogicalKind::Array,     false},
    {1009u, "text[]",          PgLogicalKind::Array,     false},
    {1014u, "bpchar[]",        PgLogicalKind::Array,     false},
    {1015u, "varchar[]",       PgLogicalKind::Array,     false},
    {1016u, "int8[]",          PgLogicalKind::Array,     false},
    {1021u, "float4[]",        PgLogicalKind::Array,     false},
    {1022u, "float8[]",        PgLogicalKind::Array,     false},
    {1028u, "oid[]",           PgLogicalKind::Array,     false},
    {1041u, "inet[]",          PgLogicalKind::Array,     false},
    {1115u, "timestamp[]",     PgLogicalKind::Array,     false},
    {1182u, "date[]",          PgLogicalKind::Array,     false},
    {1183u, "time[]",          PgLogicalKind::Array,     false},
    {1185u, "timestamptz[]",   PgLogicalKind::Array,     false},
    {1231u, "numeric[]",       PgLogicalKind::Array,     false},
    {2951u, "uuid[]",          PgLogicalKind::Array,     false},
    {3807u, "jsonb[]",         PgLogicalKind::Array,     false},
});

[[nodiscard]] constexpr PgTypeEntry lookup_pg_oid(uint32_t oid) noexcept {
    for (const PgTypeEntry& entry : kPgBuiltinTypeMap) {
        if (entry.oid == oid) {
            return entry;
        }
    }
    return PgTypeEntry{oid, "unknown", PgLogicalKind::Unknown, false};
}

[[nodiscard]] constexpr core::LogicalTypeKind to_logical_type(PgLogicalKind k) noexcept {
    switch (k) {
    case PgLogicalKind::Bool:      return core::LogicalTypeKind::Bool;
    case PgLogicalKind::Int:       return core::LogicalTypeKind::Int;
    case PgLogicalKind::Float:     return core::LogicalTypeKind::Float;
    case PgLogicalKind::Numeric:   return core::LogicalTypeKind::Numeric;
    case PgLogicalKind::Text:      return core::LogicalTypeKind::Text;
    case PgLogicalKind::Json:      return core::LogicalTypeKind::Json;
    case PgLogicalKind::Xml:       return core::LogicalTypeKind::Xml;
    case PgLogicalKind::Binary:    return core::LogicalTypeKind::Binary;
    case PgLogicalKind::Date:      return core::LogicalTypeKind::Date;
    case PgLogicalKind::Time:      return core::LogicalTypeKind::Time;
    case PgLogicalKind::Timestamp: return core::LogicalTypeKind::Timestamp;
    case PgLogicalKind::Uuid:      return core::LogicalTypeKind::Uuid;
    case PgLogicalKind::Array:     return core::LogicalTypeKind::Array;
    case PgLogicalKind::Unknown:   return core::LogicalTypeKind::Unknown;
    }
    return core::LogicalTypeKind::Unknown;
}

/// Classifies extension/custom PostgreSQL type names resolved from pg_type (e.g. "citext", "_citext", "my_enum[]").
[[nodiscard]] inline core::LogicalTypeKind classify_pg_type_name(
    std::string_view type_name,
    bool& may_hold_json_out) noexcept {
    may_hold_json_out = false;
    std::string lower;
    lower.reserve(type_name.size());
    for (const unsigned char ch : type_name) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (lower.ends_with("[]") || lower.starts_with('_')) {
        return core::LogicalTypeKind::Array;
    }
    if (lower == "citext" || lower == "text" || lower == "varchar" ||
        lower == "character varying" || lower == "bpchar" || lower == "char") {
        may_hold_json_out = true;
        return core::LogicalTypeKind::Text;
    }
    if (lower == "json" || lower == "jsonb") {
        may_hold_json_out = true;
        return core::LogicalTypeKind::Json;
    }
    if (lower == "bytea") {
        return core::LogicalTypeKind::Binary;
    }
    if (lower == "money" || lower == "numeric" || lower == "decimal") {
        return core::LogicalTypeKind::Numeric;
    }
    if (lower == "xml") {
        return core::LogicalTypeKind::Xml;
    }
    if (lower == "uuid") {
        return core::LogicalTypeKind::Uuid;
    }
    if (lower.find("range") != std::string::npos ||
        lower == "point" || lower == "lseg" || lower == "path" ||
        lower == "box" || lower == "polygon" || lower == "line" || lower == "circle" ||
        lower == "inet" || lower == "cidr" || lower == "macaddr" || lower == "macaddr8" ||
        lower == "interval") {
        return core::LogicalTypeKind::Text;
    }
    return core::LogicalTypeKind::Unknown;
}

} // namespace capybaradb::drivers
