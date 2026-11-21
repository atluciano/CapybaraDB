#pragma once

#include "core/config/query_options.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace capybaradb::core {

/// Stored by numeric value in environment.json (0 = Grid, 1 = Text, 2 = File).
enum class ResultsDestination : int32_t {
    Grid = 0,
    Text = 1,
    File = 2,
};

struct ScreenRect {
    int left   = 0;
    int top    = 0;
    int width  = 0;
    int height = 0;
};

struct WindowPlacement {
    int  left           = 0;
    int  top            = 0;
    int  width          = 1440;
    int  height         = 900;
    bool maximized      = false;
    int  explorer_width = 300;
};

struct EnvironmentDocument {
    static constexpr int kCurrentVersion       = 1;
    static constexpr int kDefaultSelectTopRows = 1000;
    static constexpr int kDefaultEditTopRows   = 200;
    static constexpr int kDefaultExplorerWidth = 300;

    int                            version                     = kCurrentVersion;
    bool                           restore_window_placement    = true;
    std::optional<WindowPlacement> window                      = std::nullopt;
    int                            select_top_rows             = kDefaultSelectTopRows;
    int                            edit_top_rows               = kDefaultEditTopRows;
    ResultsDestination             default_results_destination = ResultsDestination::Grid;
    bool                           show_completion_time        = true;
    bool                           check_open_transactions     = true;
    QueryOptions                   default_query_options{};
    QueryHistorySettings           query_history{};

    [[nodiscard]] constexpr QueryOptions normalized_query_options() const noexcept {
        return default_query_options.normalized();
    }

    [[nodiscard]] constexpr QueryHistorySettings normalized_query_history() const noexcept {
        return query_history.normalized();
    }

    [[nodiscard]] constexpr int normalized_select_top() const noexcept {
        return select_top_rows > 0 ? select_top_rows : kDefaultSelectTopRows;
    }

    [[nodiscard]] constexpr int normalized_edit_top() const noexcept {
        return edit_top_rows > 0 ? edit_top_rows : kDefaultEditTopRows;
    }

    [[nodiscard]] constexpr ResultsDestination normalized_default_results() const noexcept {
        switch (default_results_destination) {
        case ResultsDestination::Grid:
        case ResultsDestination::Text:
        case ResultsDestination::File:
            return default_results_destination;
        }
        return ResultsDestination::Grid;
    }

    [[nodiscard]] constexpr int normalized_explorer_width() const noexcept {
        if (!window.has_value() || window->explorer_width < 120) {
            return kDefaultExplorerWidth;
        }
        const int max_allowed = (window->width > 400)
            ? std::min(600, window->width - 240)
            : 600;
        if (window->explorer_width > max_allowed) {
            return kDefaultExplorerWidth;
        }
        return window->explorer_width;
    }
};

/// Returns true if the candidate window bounds have at least a 100x50 px intersection
/// with at least one available screen geometry and meet minimum size thresholds.
[[nodiscard]] bool is_placement_visible_on_screens(
    const WindowPlacement& placement,
    std::span<const ScreenRect> screens) noexcept;

/// Clamps desired width/height into the available screen work area while keeping min bounds safe.
[[nodiscard]] ScreenRect fit_bounds_to_work_area(
    const ScreenRect& desired,
    const ScreenRect& work_area) noexcept;

/// Loads environment.json; returns defaults if the file is missing, unreadable, or invalid.
[[nodiscard]] EnvironmentDocument load_environment_options(const std::filesystem::path& path);

/// Atomically writes updated configuration via temporary file + rename.
bool save_environment_options(const std::filesystem::path& path, const EnvironmentDocument& doc);

/// Persistence for Object Explorer toolbar hidden button IDs (explorer-toolbar.json).
[[nodiscard]] std::vector<std::string> load_explorer_hidden_toolbar_ids(const std::filesystem::path& path);
bool save_explorer_hidden_toolbar_ids(
    const std::filesystem::path& path,
    std::span<const std::string> hidden_ids);

} // namespace capybaradb::core
