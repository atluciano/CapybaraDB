#include "core/config/environment_options.hpp"

#include "core/config/file_util.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace capybaradb::core {

bool is_placement_visible_on_screens(
    const WindowPlacement& placement,
    std::span<const ScreenRect> screens) noexcept {
    if (placement.width < 200 || placement.height < 150) {
        return false;
    }
    const int p_right  = placement.left + placement.width;
    const int p_bottom = placement.top + placement.height;

    for (const ScreenRect& s : screens) {
        if (s.width <= 0 || s.height <= 0) {
            continue;
        }
        const int s_right  = s.left + s.width;
        const int s_bottom = s.top + s.height;
        const int inter_w  = std::min(p_right, s_right) - std::max(placement.left, s.left);
        const int inter_h  = std::min(p_bottom, s_bottom) - std::max(placement.top, s.top);
        if (inter_w >= 100 && inter_h >= 50) {
            return true;
        }
    }
    return false;
}

ScreenRect fit_bounds_to_work_area(
    const ScreenRect& desired,
    const ScreenRect& work_area) noexcept {
    if (work_area.width <= 0 || work_area.height <= 0) {
        return desired;
    }
    const int margin = std::min(8, std::min(work_area.width, work_area.height) / 4);
    const int max_w  = std::max(1, work_area.width - 2 * margin);
    const int max_h  = std::max(1, work_area.height - 2 * margin);
    const int width  = std::min(desired.width, max_w);
    const int height = std::min(desired.height, max_h);

    const int min_x = work_area.left + margin;
    const int max_x = std::max(min_x, work_area.left + work_area.width - margin - width);
    const int min_y = work_area.top + margin;
    const int max_y = std::max(min_y, work_area.top + work_area.height - margin - height);

    return ScreenRect{
        std::clamp(desired.left, min_x, max_x),
        std::clamp(desired.top, min_y, max_y),
        width,
        height,
    };
}

EnvironmentDocument load_environment_options(const std::filesystem::path& path) {
    const auto text = read_text_file(path);
    if (!text.has_value()) {
        return EnvironmentDocument{};
    }

    const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return EnvironmentDocument{};
    }

    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() != EnvironmentDocument::kCurrentVersion) {
        return EnvironmentDocument{};
    }

    EnvironmentDocument doc;
    if (j.contains("restore_window_placement") && j["restore_window_placement"].is_boolean()) {
        doc.restore_window_placement = j["restore_window_placement"].get<bool>();
    }
    if (j.contains("select_top_rows") && j["select_top_rows"].is_number_integer()) {
        doc.select_top_rows = j["select_top_rows"].get<int>();
    }
    if (j.contains("edit_top_rows") && j["edit_top_rows"].is_number_integer()) {
        doc.edit_top_rows = j["edit_top_rows"].get<int>();
    }
    if (j.contains("default_results_destination") && j["default_results_destination"].is_number_integer()) {
        doc.default_results_destination = static_cast<ResultsDestination>(j["default_results_destination"].get<int>());
    }
    if (j.contains("show_completion_time") && j["show_completion_time"].is_boolean()) {
        doc.show_completion_time = j["show_completion_time"].get<bool>();
        doc.default_query_options.show_completion_time = doc.show_completion_time;
    }
    if (j.contains("check_open_transactions") && j["check_open_transactions"].is_boolean()) {
        doc.check_open_transactions = j["check_open_transactions"].get<bool>();
        doc.default_query_options.check_open_transactions = doc.check_open_transactions;
    }
    if (j.contains("default_query_options") && j["default_query_options"].is_object()) {
        const auto& qj = j["default_query_options"];
        auto& q = doc.default_query_options;
        if (qj.contains("execution_timeout_seconds") && qj["execution_timeout_seconds"].is_number_integer())
            q.execution_timeout_seconds = qj["execution_timeout_seconds"].get<int32_t>();
        if (qj.contains("check_open_transactions") && qj["check_open_transactions"].is_boolean()) {
            q.check_open_transactions = qj["check_open_transactions"].get<bool>();
            doc.check_open_transactions = q.check_open_transactions;
        }
        if (qj.contains("nocount") && qj["nocount"].is_boolean())
            q.nocount = qj["nocount"].get<bool>();
        if (qj.contains("show_completion_time") && qj["show_completion_time"].is_boolean()) {
            q.show_completion_time = qj["show_completion_time"].get<bool>();
            doc.show_completion_time = q.show_completion_time;
        }
        if (qj.contains("isolation") && qj["isolation"].is_number_integer())
            q.isolation = static_cast<QueryIsolationLevel>(qj["isolation"].get<int32_t>());
        if (qj.contains("lock_timeout_ms") && qj["lock_timeout_ms"].is_number_integer())
            q.lock_timeout_ms = qj["lock_timeout_ms"].get<int32_t>();
        if (qj.contains("plan_warning_ratio") && qj["plan_warning_ratio"].is_number_integer())
            q.plan_warning_ratio = qj["plan_warning_ratio"].get<int32_t>();
        if (qj.contains("plan_warning_min_rows") && qj["plan_warning_min_rows"].is_number_integer())
            q.plan_warning_min_rows = qj["plan_warning_min_rows"].get<int32_t>();
        if (qj.contains("retain_line_breaks") && qj["retain_line_breaks"].is_boolean())
            q.retain_line_breaks = qj["retain_line_breaks"].get<bool>();
        if (qj.contains("grid_discard_results") && qj["grid_discard_results"].is_boolean())
            q.grid_discard_results = qj["grid_discard_results"].get<bool>();
        if (qj.contains("display_json_as_link") && qj["display_json_as_link"].is_boolean())
            q.display_json_as_link = qj["display_json_as_link"].get<bool>();
        if (qj.contains("grid_page_rows") && qj["grid_page_rows"].is_number_integer())
            q.grid_page_rows = qj["grid_page_rows"].get<int32_t>();
        if (qj.contains("max_arena_mib") && qj["max_arena_mib"].is_number_unsigned())
            q.max_arena_mib = qj["max_arena_mib"].get<uint32_t>();
        if (qj.contains("text_format") && qj["text_format"].is_number_integer())
            q.text_format = static_cast<TextOutputFormat>(qj["text_format"].get<int32_t>());
        if (qj.contains("custom_delimiter") && qj["custom_delimiter"].is_string()) {
            const std::string s = qj["custom_delimiter"].get<std::string>();
            q.custom_delimiter = (s.size() == 1) ? s[0] : '\0';
        }
        if (qj.contains("text_include_headers") && qj["text_include_headers"].is_boolean())
            q.text_include_headers = qj["text_include_headers"].get<bool>();
        if (qj.contains("text_include_query") && qj["text_include_query"].is_boolean())
            q.text_include_query = qj["text_include_query"].get<bool>();
        if (qj.contains("text_right_align_numeric") && qj["text_right_align_numeric"].is_boolean())
            q.text_right_align_numeric = qj["text_right_align_numeric"].get<bool>();
        if (qj.contains("text_discard_results") && qj["text_discard_results"].is_boolean())
            q.text_discard_results = qj["text_discard_results"].get<bool>();
        if (qj.contains("text_max_characters") && qj["text_max_characters"].is_number_unsigned())
            q.text_max_characters = qj["text_max_characters"].get<uint32_t>();
        q.normalize_in_place();
    }
    if (j.contains("query_history") && j["query_history"].is_object()) {
        const auto& hj = j["query_history"];
        auto& h = doc.query_history;
        if (hj.contains("enabled") && hj["enabled"].is_boolean())
            h.enabled = hj["enabled"].get<bool>();
        if (hj.contains("retention_days") && hj["retention_days"].is_number_integer())
            h.retention_days = hj["retention_days"].get<int32_t>();
        if (hj.contains("max_executions") && hj["max_executions"].is_number_integer())
            h.max_executions = hj["max_executions"].get<int32_t>();
        if (hj.contains("max_sql_kib") && hj["max_sql_kib"].is_number_integer())
            h.max_sql_kib = hj["max_sql_kib"].get<int32_t>();
        h.normalize_in_place();
    }
    if (j.contains("window") && j["window"].is_object()) {
        const auto& wj = j["window"];
        if (wj.contains("left") && wj["left"].is_number_integer() &&
            wj.contains("top") && wj["top"].is_number_integer() &&
            wj.contains("width") && wj["width"].is_number_integer() &&
            wj.contains("height") && wj["height"].is_number_integer()) {
            WindowPlacement wp;
            wp.left   = wj["left"].get<int>();
            wp.top    = wj["top"].get<int>();
            wp.width  = wj["width"].get<int>();
            wp.height = wj["height"].get<int>();
            if (wj.contains("maximized") && wj["maximized"].is_boolean()) {
                wp.maximized = wj["maximized"].get<bool>();
            }
            if (wj.contains("explorer_width") && wj["explorer_width"].is_number_integer()) {
                wp.explorer_width = wj["explorer_width"].get<int>();
            }
            doc.window = wp;
        }
    }
    return doc;
}

bool save_environment_options(const std::filesystem::path& path, const EnvironmentDocument& doc) {
    QueryOptions q = doc.default_query_options.normalized();
    const QueryHistorySettings h = doc.query_history.normalized();
    const bool show_completion = q.show_completion_time;
    const bool check_open_tx   = q.check_open_transactions;

    const char delim_str[2] = {q.custom_delimiter, '\0'};
    nlohmann::json j = {
        {"version", EnvironmentDocument::kCurrentVersion},
        {"restore_window_placement", doc.restore_window_placement},
        {"select_top_rows", doc.normalized_select_top()},
        {"edit_top_rows", doc.normalized_edit_top()},
        {"default_results_destination", static_cast<int32_t>(doc.normalized_default_results())},
        {"show_completion_time", show_completion},
        {"check_open_transactions", check_open_tx},
        {"default_query_options", {
            {"execution_timeout_seconds", q.execution_timeout_seconds},
            {"check_open_transactions", check_open_tx},
            {"nocount", q.nocount},
            {"show_completion_time", show_completion},
            {"isolation", static_cast<int32_t>(q.isolation)},
            {"lock_timeout_ms", q.lock_timeout_ms},
            {"plan_warning_ratio", q.plan_warning_ratio},
            {"plan_warning_min_rows", q.plan_warning_min_rows},
            {"retain_line_breaks", q.retain_line_breaks},
            {"grid_discard_results", q.grid_discard_results},
            {"display_json_as_link", q.display_json_as_link},
            {"grid_page_rows", q.grid_page_rows},
            {"max_arena_mib", q.max_arena_mib},
            {"text_format", static_cast<int32_t>(q.text_format)},
            {"custom_delimiter", std::string(delim_str)},
            {"text_include_headers", q.text_include_headers},
            {"text_include_query", q.text_include_query},
            {"text_right_align_numeric", q.text_right_align_numeric},
            {"text_discard_results", q.text_discard_results},
            {"text_max_characters", q.text_max_characters},
        }},
        {"query_history", {
            {"enabled", h.enabled},
            {"retention_days", h.retention_days},
            {"max_executions", h.max_executions},
            {"max_sql_kib", h.max_sql_kib},
        }},
    };
    if (doc.window.has_value()) {
        const WindowPlacement& wp = *doc.window;
        j["window"] = {
            {"left", wp.left},
            {"top", wp.top},
            {"width", wp.width},
            {"height", wp.height},
            {"maximized", wp.maximized},
            {"explorer_width", wp.explorer_width >= 120 ? wp.explorer_width : EnvironmentDocument::kDefaultExplorerWidth},
        };
    }

    return atomic_write_utf8(path, j.dump(2));
}

std::vector<std::string> load_explorer_hidden_toolbar_ids(const std::filesystem::path& path) {
    const auto text = read_text_file(path);
    if (!text.has_value()) {
        return {};
    }
    const nlohmann::json j = nlohmann::json::parse(*text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        return {};
    }
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() != 1) {
        return {};
    }
    if (!j.contains("hidden") || !j["hidden"].is_array()) {
        return {};
    }
    std::vector<std::string> out;
    for (const auto& item : j["hidden"]) {
        if (item.is_string()) {
            out.push_back(item.get<std::string>());
        }
    }
    return out;
}

bool save_explorer_hidden_toolbar_ids(
    const std::filesystem::path& path,
    std::span<const std::string> hidden_ids) {
    std::vector<std::string> sorted(hidden_ids.begin(), hidden_ids.end());
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    nlohmann::json j = {
        {"version", 1},
        {"hidden", sorted},
    };
    return atomic_write_utf8(path, j.dump(2));
}

} // namespace capybaradb::core
