#pragma once

#include "core/completion/sql_completion.hpp"

#include <QFrame>
#include <QString>
#include <optional>
#include <vector>

class QLabel;
class QListWidget;

namespace capybaradb::ui {

/// Non-focus-stealing accent-insensitive IntelliSense completion popup for SqlCodeEditor.
class CompletionPopupWidget final : public QFrame {
    Q_OBJECT

public:
    explicit CompletionPopupWidget(QWidget* parent = nullptr);

    void open_at(
        const QPoint& global_pos,
        int replace_start_char_pos,
        const QString& initial_prefix,
        std::vector<core::SqlCompletionItem> all_items);

    void update_filter(const QString& prefix);
    void move_selection(int delta);
    void close_popup();

    [[nodiscard]] bool is_open() const noexcept { return isVisible(); }
    [[nodiscard]] int replace_start_char_pos() const noexcept { return replace_start_; }
    [[nodiscard]] bool has_explicit_choice() const noexcept { return explicit_choice_; }
    [[nodiscard]] std::optional<core::SqlCompletionItem> current_item() const;
    [[nodiscard]] int visible_count() const;
    [[nodiscard]] QListWidget* list_widget() const noexcept { return list_; }

signals:
    void item_accepted(const QString& text, int replace_start_char_pos);

private:
    void rebuild_list(const QString& prefix);
    void update_description();
    static QIcon icon_for_kind(core::SqlCompletionKind kind);

    std::vector<core::SqlCompletionItem> all_items_;
    std::vector<core::SqlCompletionItem> filtered_items_;
    int                                  replace_start_   = 0;
    bool                                 explicit_choice_ = false;

    QListWidget* list_ = nullptr;
    QLabel*      desc_ = nullptr;
};

} // namespace capybaradb::ui
