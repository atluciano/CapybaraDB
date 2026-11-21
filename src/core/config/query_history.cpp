#include "core/config/query_history.hpp"

#include "core/config/connection_profile.hpp"
#include "core/config/file_util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace capybaradb::core {
namespace {

// Well-known deterministic entropy GUID for CapybaraDB Query History encryption.
constexpr std::string_view kHistoryEntropyGuid = "7d9c4b1e-3a28-4f65-9e01-2c8a5f6b7d42";

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string sha256_hex_upper(std::string_view data) {
    static constexpr std::array<uint32_t, 64> kK = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    auto rotr = [](uint32_t x, uint32_t n) noexcept -> uint32_t {
        return (x >> n) | (x << (32u - n));
    };

    std::array<uint32_t, 8> h = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    const uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8ull;
    const size_t rem = (data.size() + 1u + 8u) % 64u;
    const size_t pad_zeros = (rem == 0u) ? 0u : (64u - rem);
    std::vector<uint8_t> buf;
    buf.reserve(data.size() + 1u + pad_zeros + 8u);
    buf.insert(buf.end(), reinterpret_cast<const uint8_t*>(data.data()), reinterpret_cast<const uint8_t*>(data.data()) + data.size());
    buf.push_back(0x80u);
    buf.insert(buf.end(), pad_zeros, 0u);
    for (int shift = 56; shift >= 0; shift -= 8) {
        buf.push_back(static_cast<uint8_t>((bit_len >> shift) & 0xFFu));
    }

    uint32_t w[64]{};
    for (size_t off = 0; off < buf.size(); off += 64u) {
        for (int i = 0; i < 16; ++i) {
            const size_t p = off + static_cast<size_t>(i) * 4u;
            w[i] = (static_cast<uint32_t>(buf[p]) << 24u) |
                   (static_cast<uint32_t>(buf[p + 1]) << 16u) |
                   (static_cast<uint32_t>(buf[p + 2]) << 8u) |
                   static_cast<uint32_t>(buf[p + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1    = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch    = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + S1 + ch + kK[static_cast<size_t>(i)] + w[i];
            const uint32_t S0    = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;

            hh = g;
            g  = f;
            f  = e;
            e  = d + temp1;
            d  = c;
            c  = b;
            b  = a;
            a  = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out(64, '0');
    for (int i = 0; i < 8; ++i) {
        const uint32_t v = h[static_cast<size_t>(i)];
        for (int n = 0; n < 8; ++n) {
            out[static_cast<size_t>(i * 8 + n)] = kHex[(v >> (28 - n * 4)) & 0xFu];
        }
    }
    return out;
}

bool is_pinned(const std::vector<QueryHistoryKey>& pins, const QueryHistoryKey& k) {
    return std::find(pins.begin(), pins.end(), k) != pins.end();
}

std::string key_string(const QueryHistoryKey& k) {
    return k.server + "\x1f" + std::to_string(k.port) + "\x1f" + k.database + "\x1f" + k.hash;
}

bool save_query_history_snapshot(const std::filesystem::path& path, const QueryHistorySnapshot& snapshot) {
    nlohmann::json pins_json = nlohmann::json::array();
    for (const auto& k : snapshot.pins) {
        pins_json.push_back({
            {"server", k.server},
            {"port", k.port},
            {"database", k.database},
            {"hash", k.hash},
        });
    }

    nlohmann::json entries_json = nlohmann::json::array();
    for (const auto& e : snapshot.entries) {
        nlohmann::json item = {
            {"id", e.id},
            {"started_utc_ms", e.started_utc_ms},
            {"duration_ms", e.duration_ms},
            {"engine", e.engine},
            {"server", e.server},
            {"port", e.port},
            {"database", e.database},
            {"user", e.user},
            {"sql", e.sql},
            {"original_length", e.original_length},
            {"hash", e.hash},
            {"origin", static_cast<int32_t>(e.origin)},
            {"outcome", static_cast<int32_t>(e.outcome)},
        };
        if (e.rows_returned.has_value()) item["rows_returned"] = *e.rows_returned;
        if (e.rows_affected.has_value()) item["rows_affected"] = *e.rows_affected;
        if (e.sql_state.has_value()) item["sql_state"] = *e.sql_state;
        if (e.error.has_value()) item["error"] = *e.error;
        entries_json.push_back(std::move(item));
    }

    nlohmann::json root = {
        {"version", 1},
        {"pins", std::move(pins_json)},
        {"entries", std::move(entries_json)},
    };
    std::string clear = root.dump();
    const std::string cipher = protect_secret(clear, kHistoryEntropyGuid);
    secure_wipe(clear);
    if (cipher.empty()) {
        return false;
    }
    return atomic_write_utf8(path, cipher);
}

void apply_query_history_retention(
    QueryHistorySnapshot& snapshot,
    const QueryHistorySettings& raw_settings,
    int64_t now_utc_ms) {
    const QueryHistorySettings settings = raw_settings.normalized();
    if (now_utc_ms <= 0) {
        now_utc_ms = current_utc_ms();
    }

    // Track newest entry ID per pinned key so the newest execution of a pinned query is ALWAYS kept.
    std::unordered_map<std::string, std::string> pinned_newest_id;
    std::unordered_map<std::string, int64_t> pinned_newest_ts;
    for (const auto& e : snapshot.entries) {
        const QueryHistoryKey k = e.key();
        if (!is_pinned(snapshot.pins, k)) {
            continue;
        }
        const std::string ks = key_string(k);
        auto it = pinned_newest_ts.find(ks);
        if (it == pinned_newest_ts.end() || e.started_utc_ms > it->second ||
            (e.started_utc_ms == it->second && e.id > pinned_newest_id[ks])) {
            pinned_newest_ts[ks] = e.started_utc_ms;
            pinned_newest_id[ks] = e.id;
        }
    }

    auto is_protected_pin_latest = [&](const QueryHistoryEntry& e) {
        const std::string ks = key_string(e.key());
        auto it = pinned_newest_id.find(ks);
        return it != pinned_newest_id.end() && it->second == e.id;
    };

    // 1. Age retention pruning
    const int64_t cutoff_ms = now_utc_ms - static_cast<int64_t>(settings.retention_days) * 86400LL * 1000LL;
    std::erase_if(snapshot.entries, [&](const QueryHistoryEntry& e) {
        return e.started_utc_ms < cutoff_ms && !is_protected_pin_latest(e);
    });

    // 2. Count limit pruning: when exceeding max_executions, remove oldest unpinned down to 95%
    const size_t max_exec = static_cast<size_t>(settings.max_executions);
    if (snapshot.entries.size() > max_exec) {
        const size_t target = (std::max<size_t>)(1u, (max_exec * 95u) / 100u);
        std::sort(snapshot.entries.begin(), snapshot.entries.end(), [](const QueryHistoryEntry& a, const QueryHistoryEntry& b) {
            if (a.started_utc_ms != b.started_utc_ms) return a.started_utc_ms > b.started_utc_ms;
            return a.id > b.id;
        });
        std::vector<QueryHistoryEntry> kept;
        kept.reserve(snapshot.entries.size());
        size_t protected_cnt = 0;
        for (const auto& e : snapshot.entries) {
            if (is_protected_pin_latest(e)) ++protected_cnt;
        }
        size_t unpinned_budget = (target > protected_cnt) ? (target - protected_cnt) : 0u;
        for (auto& e : snapshot.entries) {
            if (is_protected_pin_latest(e)) {
                kept.push_back(std::move(e));
            } else if (unpinned_budget > 0) {
                --unpinned_budget;
                kept.push_back(std::move(e));
            }
        }
        snapshot.entries = std::move(kept);
    }

    // Remove orphaned pins that no longer have any entry
    std::erase_if(snapshot.pins, [&](const QueryHistoryKey& pk) {
        return std::none_of(snapshot.entries.begin(), snapshot.entries.end(), [&](const QueryHistoryEntry& e) {
            return e.key() == pk;
        });
    });
}

} // namespace

std::string QueryHistoryEntry::reopen_text() const {
    if (!truncated()) {
        return sql;
    }
    return sql + "\n-- Truncated by CapybaraDB Query History: text was cut at " +
           std::to_string(sql.size()) + " of " + std::to_string(original_length) +
           " bytes when it was recorded.";
}

std::string normalize_query_history_sql(std::string_view sql) {
    std::string out;
    out.reserve(sql.size());
    bool space = false;
    for (const unsigned char ch : sql) {
        if (std::isspace(ch) != 0) {
            space = !out.empty();
            continue;
        }
        if (space) {
            out.push_back(' ');
            space = false;
        }
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

std::string hash_query_history_sql(std::string_view sql) {
    return sha256_hex_upper(normalize_query_history_sql(sql));
}

std::string cut_utf8_bytes(std::string_view text, size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0u) == 0x80u) {
        --n;
    }
    return std::string(text.substr(0, n));
}

std::string preview_query_history_sql(std::string_view sql, size_t max_bytes) {
    const std::string flat = normalize_query_history_sql(sql);
    if (flat.size() <= max_bytes) {
        return flat;
    }
    return cut_utf8_bytes(flat, max_bytes) + "\xE2\x80\xA6";
}

int64_t current_utc_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

QueryHistorySnapshot load_query_history(const std::filesystem::path& path) {
    const auto cipher = read_text_file(path);
    if (!cipher.has_value() || cipher->empty()) {
        return {};
    }
    auto clear = unprotect_secret(*cipher, kHistoryEntropyGuid);
    if (!clear.has_value() || clear->empty()) {
        return {};
    }

    const nlohmann::json j = nlohmann::json::parse(*clear, nullptr, false);
    secure_wipe(*clear);
    if (j.is_discarded() || !j.is_object()) {
        return {};
    }
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() != 1) {
        return {};
    }

    QueryHistorySnapshot snap;
    if (j.contains("pins") && j["pins"].is_array()) {
        for (const auto& pj : j["pins"]) {
            if (!pj.is_object()) continue;
            QueryHistoryKey k;
            k.server   = pj.value("server", std::string{});
            k.port     = pj.value("port", 5432);
            k.database = pj.value("database", std::string{});
            k.hash     = pj.value("hash", std::string{});
            if (!k.server.empty() && k.port >= 1 && k.port <= 65535 && k.hash.size() == 64) {
                snap.pins.push_back(std::move(k));
            }
        }
    }
    if (j.contains("entries") && j["entries"].is_array()) {
        for (const auto& ej : j["entries"]) {
            if (!ej.is_object()) continue;
            QueryHistoryEntry e;
            e.id              = ej.value("id", std::string{});
            e.started_utc_ms  = ej.value("started_utc_ms", int64_t{0});
            e.duration_ms     = ej.value("duration_ms", int64_t{0});
            e.engine          = ej.value("engine", std::string{"PostgreSql"});
            e.server          = ej.value("server", std::string{});
            e.port            = ej.value("port", 5432);
            e.database        = ej.value("database", std::string{});
            e.user            = ej.value("user", std::string{});
            e.sql             = ej.value("sql", std::string{});
            e.original_length = ej.value("original_length", e.sql.size());
            e.hash            = ej.value("hash", std::string{});
            e.origin          = static_cast<QueryHistoryOrigin>(ej.value("origin", 0));
            e.outcome         = static_cast<QueryHistoryOutcome>(ej.value("outcome", 0));
            if (ej.contains("rows_returned") && ej["rows_returned"].is_number_integer()) {
                e.rows_returned = ej["rows_returned"].get<int64_t>();
            }
            if (ej.contains("rows_affected") && ej["rows_affected"].is_number_integer()) {
                e.rows_affected = ej["rows_affected"].get<int64_t>();
            }
            if (ej.contains("sql_state") && ej["sql_state"].is_string()) {
                e.sql_state = ej["sql_state"].get<std::string>();
            }
            if (ej.contains("error") && ej["error"].is_string()) {
                e.error = ej["error"].get<std::string>();
            }
            if (!e.id.empty() && !e.server.empty() && e.port >= 1 && e.port <= 65535 && e.hash.size() == 64) {
                snap.entries.push_back(std::move(e));
            }
        }
    }
    return snap;
}

bool record_query_history_entry(
    const std::filesystem::path& path,
    const QueryHistoryCapture& capture,
    const QueryHistorySettings& raw_settings,
    int64_t now_utc_ms) {
    const QueryHistorySettings settings = raw_settings.normalized();
    if (!settings.enabled) {
        return false;
    }
    const std::string normalized = normalize_query_history_sql(capture.sql);
    if (normalized.empty() || capture.server.empty() || capture.port < 1 || capture.port > 65535) {
        return false;
    }

    QueryHistoryEntry entry;
    entry.id              = generate_uuid();
    entry.started_utc_ms  = capture.started_utc_ms > 0 ? capture.started_utc_ms : current_utc_ms();
    entry.duration_ms     = (std::max<int64_t>)(0, capture.duration_ms);
    entry.engine          = capture.engine.empty() ? std::string{"PostgreSql"} : capture.engine;
    entry.server          = cut_utf8_bytes(to_lower_ascii(capture.server), QueryHistoryEntry::kMaxNameChars);
    entry.port            = capture.port;
    entry.database        = cut_utf8_bytes(capture.database, QueryHistoryEntry::kMaxNameChars);
    entry.user            = cut_utf8_bytes(capture.user, QueryHistoryEntry::kMaxNameChars);
    entry.original_length = capture.sql.size();
    entry.sql             = cut_utf8_bytes(capture.sql, settings.max_sql_bytes());
    entry.hash            = hash_query_history_sql(capture.sql);
    entry.origin          = capture.origin;
    entry.outcome         = capture.outcome;
    if (capture.rows_returned.has_value() && *capture.rows_returned >= 0) {
        entry.rows_returned = *capture.rows_returned;
    }
    if (capture.rows_affected.has_value() && *capture.rows_affected >= 0) {
        entry.rows_affected = *capture.rows_affected;
    }
    if (capture.sql_state.has_value() && !capture.sql_state->empty() && capture.sql_state->size() <= 8) {
        entry.sql_state = *capture.sql_state;
    }
    if (capture.error.has_value() && !capture.error->empty()) {
        entry.error = cut_utf8_bytes(*capture.error, QueryHistoryEntry::kMaxErrorChars);
    }

    QueryHistorySnapshot snap = load_query_history(path);
    snap.entries.push_back(std::move(entry));
    apply_query_history_retention(snap, settings, now_utc_ms > 0 ? now_utc_ms : current_utc_ms());
    return save_query_history_snapshot(path, snap);
}

bool prune_query_history(
    const std::filesystem::path& path,
    const QueryHistorySettings& settings,
    int64_t now_utc_ms) {
    QueryHistorySnapshot snap = load_query_history(path);
    const size_t before_e = snap.entries.size();
    const size_t before_p = snap.pins.size();
    apply_query_history_retention(snap, settings, now_utc_ms > 0 ? now_utc_ms : current_utc_ms());
    if (snap.entries.size() == before_e && snap.pins.size() == before_p) {
        return true;
    }
    return save_query_history_snapshot(path, snap);
}

bool toggle_query_history_pin(
    const std::filesystem::path& path,
    const QueryHistoryKey& key) {
    QueryHistorySnapshot snap = load_query_history(path);
    auto it = std::find(snap.pins.begin(), snap.pins.end(), key);
    if (it != snap.pins.end()) {
        snap.pins.erase(it);
    } else {
        snap.pins.push_back(key);
    }
    return save_query_history_snapshot(path, snap);
}

bool delete_query_history_entry(
    const std::filesystem::path& path,
    std::string_view entry_id) {
    QueryHistorySnapshot snap = load_query_history(path);
    std::erase_if(snap.entries, [&](const QueryHistoryEntry& e) { return e.id == entry_id; });
    std::erase_if(snap.pins, [&](const QueryHistoryKey& pk) {
        return std::none_of(snap.entries.begin(), snap.entries.end(), [&](const QueryHistoryEntry& e) {
            return e.key() == pk;
        });
    });
    return save_query_history_snapshot(path, snap);
}

bool delete_query_history_group(
    const std::filesystem::path& path,
    const QueryHistoryKey& key) {
    QueryHistorySnapshot snap = load_query_history(path);
    std::erase_if(snap.entries, [&](const QueryHistoryEntry& e) { return e.key() == key; });
    std::erase(snap.pins, key);
    return save_query_history_snapshot(path, snap);
}

bool clear_query_history(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return std::filesystem::remove(path, ec) && !ec;
    }
    return true;
}

std::vector<QueryHistoryGroup> build_query_history_groups(const QueryHistorySnapshot& snapshot) {
    std::unordered_map<std::string, size_t> index_by_key;
    std::vector<QueryHistoryGroup> groups;

    for (const auto& e : snapshot.entries) {
        const QueryHistoryKey k = e.key();
        const std::string ks = key_string(k);
        auto it = index_by_key.find(ks);
        if (it == index_by_key.end()) {
            index_by_key.emplace(ks, groups.size());
            QueryHistoryGroup g;
            g.key    = k;
            g.pinned = is_pinned(snapshot.pins, k);
            g.executions.push_back(e);
            groups.push_back(std::move(g));
        } else {
            groups[it->second].executions.push_back(e);
        }
    }

    for (auto& g : groups) {
        std::sort(g.executions.begin(), g.executions.end(), [](const QueryHistoryEntry& a, const QueryHistoryEntry& b) {
            if (a.started_utc_ms != b.started_utc_ms) return a.started_utc_ms > b.started_utc_ms;
            return a.id > b.id;
        });
    }

    std::sort(groups.begin(), groups.end(), [](const QueryHistoryGroup& a, const QueryHistoryGroup& b) {
        if (a.pinned != b.pinned) return a.pinned && !b.pinned;
        return a.latest().started_utc_ms > b.latest().started_utc_ms;
    });

    return groups;
}

std::vector<QueryHistoryGroup> load_query_history_groups(const std::filesystem::path& path) {
    return build_query_history_groups(load_query_history(path));
}

} // namespace capybaradb::core
