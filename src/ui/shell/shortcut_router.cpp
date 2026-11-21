#include "ui/shell/shortcut_router.hpp"

#include "ui/explorer/object_explorer_panel.hpp"

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QWidget>

namespace capybaradb::ui {

ShortcutRouter::ShortcutRouter(QWidget* root_window, ObjectExplorerPanel* explorer, QObject* parent)
    : QObject(parent)
    , root_window_(root_window)
    , explorer_(explorer) {}

void ShortcutRouter::clear_chord() noexcept {
    active_chord_ = ChordKind::None;
    chord_deadline_ = {};
}

bool ShortcutRouter::is_chord_alive() const noexcept {
    return active_chord_ != ChordKind::None &&
           std::chrono::steady_clock::now() <= chord_deadline_;
}

bool ShortcutRouter::is_focus_in_explorer() const noexcept {
    if (explorer_ == nullptr) {
        return false;
    }
    QWidget* fw = QApplication::focusWidget();
    return fw != nullptr && (fw == explorer_ || explorer_->isAncestorOf(fw));
}

bool ShortcutRouter::eventFilter(QObject* watched, QEvent* event) {
    if (event == nullptr) {
        return QObject::eventFilter(watched, event);
    }

    const QEvent::Type type = event->type();
    if (type != QEvent::KeyPress && type != QEvent::ShortcutOverride) {
        return QObject::eventFilter(watched, event);
    }

    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr || root_window_ == nullptr) {
        return false;
    }
    if (widget != root_window_ && !root_window_->isAncestorOf(widget)) {
        return false;
    }

    auto* key_event = static_cast<QKeyEvent*>(event);
    const bool is_override = (type == QEvent::ShortcutOverride);
    if (process_key_event(key_event, is_override)) {
        if (is_override) {
            key_event->accept();
        }
        return true;
    }
    return false;
}

bool ShortcutRouter::process_key_event(QKeyEvent* event, bool is_override) {
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers() &
        (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);

    // Ignore bare modifier presses so chords stay alive while Ctrl is held.
    if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta) {
        return false;
    }

    // Global Alt+Break / Ctrl+Break -> Cancel Executing Query
    if ((key == Qt::Key_Pause || key == Qt::Key_Cancel) &&
        ((mods & Qt::AltModifier) || (mods & Qt::ControlModifier))) {
        if (!is_override) {
            clear_chord();
            emit cancel_requested();
        }
        return true;
    }

    // Active 2-key chord completion (2000 ms window)
    if (active_chord_ != ChordKind::None) {
        const bool alive = is_chord_alive();
        const ChordKind pending = active_chord_;
        if (!is_override) {
            clear_chord();
        }

        if (key == Qt::Key_Escape && mods == Qt::NoModifier) {
            if (!is_override) {
                emit status_hint(QStringLiteral("Key chord canceled."));
            }
            return true;
        }

        if (alive && mods == Qt::ControlModifier) {
            if (pending == ChordKind::CtrlK && (key == Qt::Key_C || key == Qt::Key_U)) {
                if (!is_override) {
                    if (key == Qt::Key_C) {
                        emit comment_selection_requested();
                    } else {
                        emit uncomment_selection_requested();
                    }
                }
                return true;
            }
            if (pending == ChordKind::CtrlB && key == Qt::Key_I) {
                if (!is_override) {
                    emit toggle_intellisense_requested();
                }
                return true;
            }
        }
        if (is_override) {
            return false;
        }
    }

    // Start 2-key chord: Ctrl+K or Ctrl+B
    if (mods == Qt::ControlModifier && (key == Qt::Key_K || key == Qt::Key_B)) {
        if (!is_override) {
            active_chord_ = (key == Qt::Key_K) ? ChordKind::CtrlK : ChordKind::CtrlB;
            chord_deadline_ = std::chrono::steady_clock::now() + kChordWindow;
            emit status_hint(key == Qt::Key_K
                ? QStringLiteral("(Ctrl+K) was pressed. Waiting for second key of chord…")
                : QStringLiteral("(Ctrl+B) was pressed. Waiting for second key of chord…"));
        }
        return true;
    }

    // Contextual F5 (Object Explorer refresh vs Query execution)
    if (key == Qt::Key_F5 && mods == Qt::NoModifier) {
        if (!is_override) {
            if (is_focus_in_explorer()) {
                emit refresh_explorer_requested();
            } else {
                emit execute_requested();
            }
        }
        return true;
    }

    // Alt+X / Ctrl+E -> Execute (unless focused inside Object Explorer)
    if ((mods == Qt::AltModifier && key == Qt::Key_X) ||
        (mods == Qt::ControlModifier && key == Qt::Key_E)) {
        if (!is_focus_in_explorer()) {
            if (!is_override) {
                emit execute_requested();
            }
            return true;
        }
    }

    // Esc in Object Explorer cancels folder loading
    if (key == Qt::Key_Escape && mods == Qt::NoModifier && is_focus_in_explorer()) {
        if (!is_override) {
            emit cancel_explorer_load_requested();
        }
        return true;
    }

    // Ctrl+R -> Toggle bottom results pane
    if (mods == Qt::ControlModifier && key == Qt::Key_R) {
        if (!is_override) {
            emit toggle_results_pane_requested();
        }
        return true;
    }

    // F8 -> Show and focus Object Explorer
    if (key == Qt::Key_F8 && mods == Qt::NoModifier) {
        if (!is_override) {
            emit show_explorer_requested();
        }
        return true;
    }

    // Ctrl+N -> New Query
    if (mods == Qt::ControlModifier && key == Qt::Key_N) {
        if (!is_override) {
            emit new_query_requested();
        }
        return true;
    }

    // Ctrl+F4 -> Close active document tab
    if (mods == Qt::ControlModifier && key == Qt::Key_F4) {
        if (!is_override) {
            emit close_active_tab_requested();
        }
        return true;
    }

    // Ctrl+I (without Ctrl+B chord) -> Generate SQL with AI
    if (mods == Qt::ControlModifier && key == Qt::Key_I) {
        if (!is_override) {
            emit generate_sql_ai_requested();
        }
        return true;
    }

    return false;
}

} // namespace capybaradb::ui
