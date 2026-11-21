#pragma once

#include <QColor>
#include <QFont>
#include <QString>
#include <QStringView>

class QApplication;

namespace capybaradb::ui::theme {

// ---------------------------------------------------------------------------
// 1.1 Shell, Docking, and Toolbars Palette (docs/PARITY_SPEC.md §1.1)
// ---------------------------------------------------------------------------
inline constexpr const char* kShellBg            = "#A8C7D9";
inline constexpr const char* kPanelBg            = "#F5FAFE";
inline constexpr const char* kToolbarBg          = "#D5E5EF";
inline constexpr const char* kSplitterBg         = "#D6E3ED";
inline constexpr const char* kMainStatusBarBg    = "#425C6C";
inline constexpr const char* kMainStatusBarFg    = "#FFFFFF";
inline constexpr const char* kPrimaryText        = "#1E1E1E";
inline constexpr const char* kDialogBg           = "#F5F5F5";
inline constexpr const char* kDialogHeaderBg     = "#E4EFF6";
inline constexpr const char* kDialogHeaderBorder = "#B5C5D2";
inline constexpr const char* kDialogTitleFg      = "#294A60";
inline constexpr const char* kMutedTextFg        = "#4B5964";
inline constexpr const char* kErrorTextFg        = "#A4262C";
inline constexpr const char* kWarningTextFg      = "#814600";
inline constexpr const char* kCategoryToggleBg   = "#EEF3FA";

// ---------------------------------------------------------------------------
// 1.2 ResultSet Grid and OutputTabs Palette (docs/PARITY_SPEC.md §1.2)
// ---------------------------------------------------------------------------
inline constexpr const char* kGridCellBg         = "#F5F5F5";
inline constexpr const char* kGridNullBg         = "#FDFBAC";
inline constexpr const char* kGridSelectionBg    = "#A5CFF1";
inline constexpr const char* kGridLine           = "#CCCEDB";
inline constexpr const char* kGridHeaderBg       = "#F0F0F0";
inline constexpr const char* kGridTopLeftBg      = "#E1E1E1";
inline constexpr const char* kGridHeaderDivider  = "#D0D0D0";
inline constexpr const char* kGridOuterBorder    = "#ABADB3";
inline constexpr const char* kGridTextFg         = "#1E1E1E";
inline constexpr const char* kGridLinkFg         = "#0066CC";
inline constexpr const char* kGridSortArrow      = "#717171";
inline constexpr const char* kGridFocusRect      = "#000000";
inline constexpr const char* kOutputTabStripBg   = "#EEEEEE";
inline constexpr const char* kOutputTabHoverBg   = "#E2E6EC";
inline constexpr const char* kOutputTabActiveBg  = "#FFFFFF";
inline constexpr const char* kOutputTabBorder    = "#CCCEDB";
inline constexpr const char* kOutputTabFg        = "#222222";

// ---------------------------------------------------------------------------
// 1.3 Query Status Bar Palette (docs/PARITY_SPEC.md §1.3)
// ---------------------------------------------------------------------------
inline constexpr const char* kQueryStatusNormalBg = "#FFF49C";
inline constexpr const char* kQueryStatusErrorBg  = "#FDE7E9";
inline constexpr const char* kQueryStatusBorder   = "#D5C777";
inline constexpr const char* kQueryStatusFg       = "#222222";
inline constexpr const char* kStatusIconSuccess   = "#107C41";
inline constexpr const char* kStatusIconRunning   = "#0078D4";
inline constexpr const char* kStatusIconError     = "#D13438";
inline constexpr const char* kStatusIconIdle      = "#717171";

// ---------------------------------------------------------------------------
// 1.4 Object Explorer, Toolbar, Syntax, and Plan Tokens (§1.4)
// ---------------------------------------------------------------------------
inline constexpr const char* kTreeExpanderBorder      = "#919191";
inline constexpr const char* kTreeExpanderGrad0       = "#FCFCFC";
inline constexpr const char* kTreeExpanderGrad1       = "#FAFBFB";
inline constexpr const char* kTreeExpanderGrad2       = "#EDEDEC";
inline constexpr const char* kTreeExpanderGrad3       = "#E3E3E3";
inline constexpr const char* kTreeExpanderMinus       = "#4B63A7";
inline constexpr const char* kTreeExpanderPlus        = "#294272";
inline constexpr const char* kTreeSelectionActiveBg   = "#0078D7";
inline constexpr const char* kTreeSelectionInactiveBg = "#CCE4F7";
inline constexpr const char* kTreeFocusAlt            = "#FF8728";

inline constexpr const char* kIconFolderTan           = "#DCB67A";
inline constexpr const char* kIconInk                 = "#536675";
inline constexpr const char* kIconBlue                = "#4E8EB8";
inline constexpr const char* kIconLight               = "#E1EFF7";
inline constexpr const char* kIconGold                = "#D0A64B";
inline constexpr const char* kIconPaper               = "#FFF5D4";
inline constexpr const char* kIconPurple              = "#8066A7";
inline constexpr const char* kIconGreen               = "#3B8751";
inline constexpr const char* kIconRed                 = "#C42B1C";

inline constexpr const char* kToolbarExecuteFill      = "#2D7D3B";
inline constexpr const char* kToolbarExecuteStroke    = "#1B5927";
inline constexpr const char* kToolbarExecuteDisFill   = "#D8DCE0";
inline constexpr const char* kToolbarExecuteDisStroke = "#9EA7B0";
inline constexpr const char* kToolbarCancelFill       = "#AB3E3E";
inline constexpr const char* kToolbarCancelStroke     = "#7A2020";
inline constexpr const char* kToolbarCancelDisFill    = "#A0A8B0";
inline constexpr const char* kToolbarCancelDisStroke  = "#8E96A0";
inline constexpr const char* kExplorerStopIdle        = "#C8C8C8";
inline constexpr const char* kExplorerStopActive      = "#A1260D";

inline constexpr const char* kSyntaxComment           = "#008000";
inline constexpr const char* kSyntaxString            = "#A31515";
inline constexpr const char* kSyntaxKeyword           = "#0000FF";
inline constexpr const char* kSyntaxLocalVar          = "#008080";
inline constexpr const char* kSyntaxGlobalVar         = "#FF00FF";
inline constexpr const char* kSyntaxBracketIdent      = "#1E1E1E";

inline constexpr const char* kPlanSelection           = "#0078D7";
inline constexpr const char* kPlanArrow               = "#A6A6A6";
inline constexpr const char* kPlanTooltipBg           = "#FFFFE1";
inline constexpr const char* kPlanTooltipBorder       = "#C9C36B";
inline constexpr const char* kPlanIconStroke          = "#2B5797";
inline constexpr const char* kPlanIconFill            = "#DDE8F5";

// ---------------------------------------------------------------------------
// 1.5 Structural Metrics & Typography (§1.5)
// ---------------------------------------------------------------------------
inline constexpr int kDefaultWindowWidthPx          = 1440;
inline constexpr int kDefaultWindowHeightPx         = 900;
inline constexpr int kMinWindowWidthPx              = 900;
inline constexpr int kMinWindowHeightPx             = 600;
inline constexpr int kUiFontSizePx                  = 12;
inline constexpr int kEditorFontSizePx              = 14;
inline constexpr int kResultsTextFontSizePx         = 13;
inline constexpr int kMessagesFontSizePx            = 12;
inline constexpr int kQueryStatusFontSizePx         = 11;
inline constexpr int kGridRowHeightPx               = 19;
inline constexpr int kGridColumnHeaderHeightPx      = 19;
inline constexpr int kQueryStatusBarHeightPx        = 24;
inline constexpr int kOutputTabStripHeightPx        = 25;
inline constexpr int kObjectExplorerInitialWidthPx  = 300;
inline constexpr int kTreeItemHeightPx              = 18;
inline constexpr int kIconSizePx                    = 16;
inline constexpr int kStatusIconSizePx              = 14;
inline constexpr int kTreeExpanderBoxPx             = 9;
inline constexpr int kSplitterWidthPx               = 5;
inline constexpr int kEditorMinHeightPx             = 100;
inline constexpr int kResultsMinHeightPx            = 200;
inline constexpr int kAvailableDbComboWidthPx       = 172;

[[nodiscard]] inline QColor color(const char* hex) {
    return QColor(QString::fromLatin1(hex));
}

/// Resolves the deterministic platform UI font:
/// Windows: "Segoe UI"
/// Linux/other: "Inter" -> "Liberation Sans" -> "Noto Sans"
[[nodiscard]] QFont resolve_ui_font(int pixel_size = kUiFontSizePx);

/// Resolves the deterministic platform monospace font:
/// Windows: "Consolas"
/// Linux/other: "JetBrains Mono" -> "Liberation Mono" -> "DejaVu Sans Mono"
[[nodiscard]] QFont resolve_mono_font(int pixel_size = kEditorFontSizePx);

/// Loads :/themes/classic_blue.qss from Qt resources.
[[nodiscard]] QString load_classic_blue_stylesheet();

/// Applies global palette, platform UI font, and Classic Blue stylesheet to QApplication.
/// Returns false if the stylesheet resource could not be loaded.
bool apply_classic_blue_theme(QApplication& app);

} // namespace capybaradb::ui::theme
