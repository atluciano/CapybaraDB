#pragma once

#include "core/result/result_set_storage.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace capybaradb::drivers {

// Microsoft ODBC Driver for SQL Server vendor-specific SQL type codes (msodbcsql.h / sqlncli.h)
inline constexpr int16_t kSqlSsVariant        = -150; // sql_variant
inline constexpr int16_t kSqlSsUdt            = -151; // CLR UDT: hierarchyid, geometry, geography
inline constexpr int16_t kSqlSsXml            = -152; // xml
inline constexpr int16_t kSqlSsTable          = -153; // table-valued parameter
inline constexpr int16_t kSqlSsTime2          = -154; // time(n)
inline constexpr int16_t kSqlSsTimestampOffset = -155; // datetimeoffset(n)

inline constexpr int16_t kOdbcTypeChar          = 1;
inline constexpr int16_t kOdbcTypeNumeric       = 2;
inline constexpr int16_t kOdbcTypeDecimal       = 3;
inline constexpr int16_t kOdbcTypeInteger       = 4;
inline constexpr int16_t kOdbcTypeSmallInt      = 5;
inline constexpr int16_t kOdbcTypeFloat         = 6;
inline constexpr int16_t kOdbcTypeReal          = 7;
inline constexpr int16_t kOdbcTypeDouble        = 8;
inline constexpr int16_t kOdbcTypeVarchar       = 12;
inline constexpr int16_t kOdbcTypeDate          = 91;
inline constexpr int16_t kOdbcTypeTime          = 92;
inline constexpr int16_t kOdbcTypeTimestamp     = 93;
inline constexpr int16_t kOdbcTypeLongVarchar   = -1;
inline constexpr int16_t kOdbcTypeBinary        = -2;
inline constexpr int16_t kOdbcTypeVarBinary     = -3;
inline constexpr int16_t kOdbcTypeLongVarBinary = -4;
inline constexpr int16_t kOdbcTypeBigInt        = -5;
inline constexpr int16_t kOdbcTypeTinyInt       = -6;
inline constexpr int16_t kOdbcTypeBit           = -7;
inline constexpr int16_t kOdbcTypeWChar         = -8;
inline constexpr int16_t kOdbcTypeWVarchar      = -9;
inline constexpr int16_t kOdbcTypeWLongVarchar  = -10;
inline constexpr int16_t kOdbcTypeGuid          = -11;

inline std::string ascii_lower_copy(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (const unsigned char ch : sv) {
        out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
}

/// Returns true for SQL Server binary / CLR UDT columns fetched as raw bytes.
[[nodiscard]] constexpr bool is_mssql_binary_type(int16_t sql_type) noexcept {
    return sql_type == kOdbcTypeBinary ||
           sql_type == kOdbcTypeVarBinary ||
           sql_type == kOdbcTypeLongVarBinary ||
           sql_type == kSqlSsUdt;
}

/// Returns true when the column MUST be bound / fetched as SQL_C_WCHAR so SQL Server
/// collations (Latin1_General, CP1252, Cyrillic, Japanese, UTF-8, etc.) are always decoded
/// losslessly into UTF-16LE and transcoded to UTF-8 without Windows ACP / codepage corruption.
[[nodiscard]] constexpr bool should_bind_mssql_as_wchar(int16_t sql_type) noexcept {
    switch (sql_type) {
    case kOdbcTypeChar:
    case kOdbcTypeVarchar:
    case kOdbcTypeLongVarchar:
    case kOdbcTypeWChar:
    case kOdbcTypeWVarchar:
    case kOdbcTypeWLongVarchar:
    case kSqlSsXml:
    case kSqlSsVariant:
        return true;
    default:
        return false;
    }
}

/// Returns true if the column requires dynamic streaming via SQLGetData (LOB, XML, sql_variant, CLR UDT).
[[nodiscard]] constexpr bool is_mssql_lob_or_dynamic_type(int16_t sql_type, uint64_t col_size) noexcept {
    if (sql_type == kOdbcTypeLongVarchar ||
        sql_type == kOdbcTypeWLongVarchar ||
        sql_type == kOdbcTypeLongVarBinary ||
        sql_type == kSqlSsXml ||
        sql_type == kSqlSsVariant ||
        sql_type == kSqlSsUdt) {
        return true;
    }
    if (col_size == 0u || col_size > 4096u) {
        if (sql_type == kOdbcTypeChar || sql_type == kOdbcTypeVarchar ||
            sql_type == kOdbcTypeWChar || sql_type == kOdbcTypeWVarchar ||
            sql_type == kOdbcTypeBinary || sql_type == kOdbcTypeVarBinary) {
            return true;
        }
    }
    return false;
}

/// Classifies SQL Server ODBC types + SQL_DESC_TYPE_NAME into LogicalTypeKind and JSON eligibility.
[[nodiscard]] inline core::LogicalTypeKind classify_mssql_column(
    int16_t sql_type,
    std::string_view type_name,
    bool& may_hold_json) noexcept {
    may_hold_json = false;
    const std::string t = ascii_lower_copy(type_name);
    if (t == "money" || t == "smallmoney") {
        return core::LogicalTypeKind::Numeric;
    }
    if (t == "uniqueidentifier") {
        return core::LogicalTypeKind::Uuid;
    }
    if (t == "datetimeoffset") {
        return core::LogicalTypeKind::Timestamp;
    }
    if (t == "xml") {
        return core::LogicalTypeKind::Xml;
    }
    if (t == "hierarchyid") {
        return core::LogicalTypeKind::Text;
    }
    if (t == "sql_variant") {
        may_hold_json = true;
        return core::LogicalTypeKind::Unknown;
    }

    switch (sql_type) {
    case kOdbcTypeBit:
        return core::LogicalTypeKind::Bool;
    case kOdbcTypeTinyInt:
    case kOdbcTypeSmallInt:
    case kOdbcTypeInteger:
    case kOdbcTypeBigInt:
        return core::LogicalTypeKind::Int;
    case kOdbcTypeReal:
    case kOdbcTypeFloat:
    case kOdbcTypeDouble:
        return core::LogicalTypeKind::Float;
    case kOdbcTypeNumeric:
    case kOdbcTypeDecimal:
        return core::LogicalTypeKind::Numeric;
    case kOdbcTypeBinary:
    case kOdbcTypeVarBinary:
    case kOdbcTypeLongVarBinary:
    case kSqlSsUdt:
        return core::LogicalTypeKind::Binary;
    case kOdbcTypeDate:
        return core::LogicalTypeKind::Date;
    case kOdbcTypeTime:
    case kSqlSsTime2:
        return core::LogicalTypeKind::Time;
    case kOdbcTypeTimestamp:
    case kSqlSsTimestampOffset:
        return core::LogicalTypeKind::Timestamp;
    case kOdbcTypeGuid:
        return core::LogicalTypeKind::Uuid;
    case kSqlSsXml:
        return core::LogicalTypeKind::Xml;
    case kSqlSsVariant:
        may_hold_json = true;
        return core::LogicalTypeKind::Unknown;
    case kOdbcTypeChar:
    case kOdbcTypeVarchar:
    case kOdbcTypeLongVarchar:
    case kOdbcTypeWChar:
    case kOdbcTypeWVarchar:
    case kOdbcTypeWLongVarchar:
        may_hold_json = true;
        return core::LogicalTypeKind::Text;
    default:
        return core::LogicalTypeKind::Unknown;
    }
}

/// Decodes Microsoft SQL Server `hierarchyid` Known-Prefix OrdPath binary bytes into canonical path text
/// (e.g. empty -> "/", 0x58 -> "/1/", 0x5B40 -> "/1/2/", 0x5460 -> "/1.5/", 0x3F -> "/-1/").
/// Returns std::nullopt if the byte sequence is not a valid hierarchyid encoding.
[[nodiscard]] inline std::optional<std::string> decode_mssql_hierarchyid(
    std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        return std::string{"/"};
    }

    const size_t total_bits = bytes.size() * 8u;
    size_t bit_pos = 0;

    auto read_bit = [&]() -> int {
        if (bit_pos >= total_bits) return -1;
        const uint8_t b = bytes[bit_pos >> 3u];
        const int val = (b >> (7u - (bit_pos & 7u))) & 1u;
        ++bit_pos;
        return val;
    };

    auto read_bits = [&](size_t count, uint64_t& out) -> bool {
        if (bit_pos + count > total_bits) return false;
        out = 0;
        for (size_t i = 0; i < count; ++i) {
            out = (out << 1u) | static_cast<uint64_t>(read_bit());
        }
        return true;
    };

    auto remaining_all_zero = [&]() -> bool {
        for (size_t p = bit_pos; p < total_bits; ++p) {
            if (((bytes[p >> 3u] >> (7u - (p & 7u))) & 1u) != 0u) {
                return false;
            }
        }
        return true;
    };

    struct Entry {
        uint32_t prefix_bits;
        uint8_t  prefix_len;
        uint8_t  offset_bits;
        int64_t  min_value;
    };

    // Known-Prefix OrdPath table (SQL Server hierarchyid specification)
    static constexpr Entry kTable[] = {
        // Positive ordinals
        {0b01u,               2,  2,  0LL},            // 0..3
        {0b100u,              3,  2,  4LL},            // 4..7
        {0b101u,              3,  3,  8LL},            // 8..15
        {0b110u,              3,  4,  16LL},           // 16..31
        {0b1110u,             4,  5,  32LL},           // 32..63
        {0b11110u,            5,  6,  64LL},           // 64..127
        {0b111110u,           6,  10, 128LL},          // 128..1151
        {0b1111110u,          7,  12, 1152LL},         // 1152..5247
        {0b11111110u,         8,  16, 5248LL},         // 5248..70783
        {0b111111110u,        9,  20, 70784LL},        // 70784..1119359
        {0b1111111110u,       10, 24, 1119360LL},      // 1119360..17896575
        {0b11111111110u,      11, 28, 17896576LL},     // 17896576..286332031
        {0b111111111110u,     12, 32, 286332032LL},    // 286332032..4581299327
        // Negative ordinals
        {0b00111u,            5,  2,  -4LL},           // -4..-1
        {0b00110u,            5,  3,  -12LL},          // -12..-5
        {0b00101u,            5,  4,  -28LL},          // -28..-13
        {0b00100u,            5,  5,  -60LL},          // -60..-29
        {0b000111u,           6,  6,  -124LL},         // -124..-61
        {0b000110u,           6,  10, -1148LL},        // -1148..-125
        {0b000101u,           6,  12, -5244LL},        // -5244..-1149
        {0b000100u,           6,  16, -70780LL},       // -70780..-5245
        {0b000011u,           6,  20, -1119356LL},     // -1119356..-70781
        {0b000010u,           6,  24, -17896572LL},    // -17896572..-1119357
        {0b0000011u,          7,  28, -286332028LL},   // -286332028..-17896573
        {0b0000010u,          7,  32, -4581299324LL},  // -4581299324..-286332029
    };

    std::string out = "/";
    bool in_dotted = false;

    while (bit_pos < total_bits) {
        if (remaining_all_zero()) {
            break;
        }

        bool matched = false;
        uint32_t acc = 0;
        const size_t start_bit = bit_pos;
        for (uint8_t len = 1; len <= 12; ++len) {
            const int b = read_bit();
            if (b < 0) return std::nullopt;
            acc = (acc << 1u) | static_cast<uint32_t>(b);
            for (const Entry& e : kTable) {
                if (e.prefix_len == len && e.prefix_bits == acc) {
                    uint64_t offset = 0;
                    if (!read_bits(e.offset_bits, offset)) {
                        return std::nullopt;
                    }
                    const int term = read_bit();
                    if (term < 0) {
                        return std::nullopt;
                    }
                    const int64_t val = e.min_value + static_cast<int64_t>(offset);
                    if (in_dotted) {
                        out.push_back('.');
                    }
                    out += std::to_string(val);
                    if (term == 1) {
                        out.push_back('/');
                        in_dotted = false;
                    } else {
                        in_dotted = true;
                    }
                    matched = true;
                    break;
                }
            }
            if (matched) break;
        }
        if (!matched || bit_pos == start_bit) {
            return std::nullopt;
        }
    }

    if (in_dotted || out.back() != '/') {
        return std::nullopt;
    }
    return out;
}

} // namespace capybaradb::drivers
