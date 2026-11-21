#pragma once

#include <QObject>
#include <chrono>

class QEvent;
class QKeyEvent;
class QWidget;

namespace capybaradb::ui {

class ObjectExplorerPanel;

class ShortcutRouter final : public QObject {
    Q_OBJECT

public:
    enum class ChordKind { None, CtrlK, CtrlB };

    explicit ShortcutRouter(QWidget* root_window, ObjectExplorerPanel* explorer, QObject* parent = nullptr);

    [[nodiscard]] ChordKind active_chord() const noexcept { return active_chord_; }
    void clear_chord() noexcept;

    /// Exposes shortcut decision for direct unit testing.
    bool process_key_event(QKeyEvent* event, bool is_override);

signals:
    void execute_requested();
    void cancel_requested();
    void refresh_explorer_requested();
    void cancel_explorer_load_requested();
    void toggle_results_pane_requested();
    void show_explorer_requested();
    void new_query_requested();
    void close_active_tab_requested();
    void comment_selection_requested();
    void uncomment_selection_requested();
    void toggle_intellisense_requested();
    void generate_sql_ai_requested();
    void status_hint(const QString& text);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool is_chord_alive() const noexcept;
    bool is_focus_in_explorer() const noexcept;

    static constexpr auto kChordWindow = std::chrono::milliseconds(2000);

    QWidget*                              root_window_ = nullptr;
    ObjectExplorerPanel*                  explorer_    = nullptr;
    ChordKind                             active_chord_ = ChordKind::None;
    std::chrono::steady_clock::time_point chord_deadline_{};
};

} // namespace capybaradb::ui
