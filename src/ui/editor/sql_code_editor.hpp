#pragma once

#include "drivers/common/db_types.hpp"

#include <QPlainTextEdit>
#include <QString>
#include <cstdint>
#include <expected>

namespace capybaradb::ui {

class SqlSyntaxHighlighter;

class SqlCodeEditor final : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit SqlCodeEditor(QWidget* parent = nullptr);

    [[nodiscard]] SqlSyntaxHighlighter* syntax_highlighter() const noexcept { return highlighter_; }
    [[nodiscard]] int line_number_area_width() const;
    [[nodiscard]] uint64_t edit_revision() const noexcept { return edit_revision_; }

    void set_engine_dialect(drivers::EngineKind engine);
    void set_highlighting_enabled(bool enabled);

    /// Comments or uncomments selected lines (or current line when no selection).
    /// Returns changed line count, or an error message on read-only / >10,000-line selection.
    [[nodiscard]] std::expected<int, QString> comment_selection(bool uncomment);

    bool navigate_to_line(int line_1based);
    bool navigate_to_char_offset(int char_offset_0based, int select_length = 0);

    /// Global screen position immediately below the current text caret for completion popups.
    [[nodiscard]] QPoint completion_popup_global_pos() const;

    /// Replaces UTF-16 character range [start_char_pos, end_char_pos] in a single undo step.
    void replace_text_range(int start_char_pos, int end_char_pos, const QString& replacement);

    /// Exact UTF-16 QString index <-> UTF-8 byte offset conversions (handles accents / surrogate pairs).
    [[nodiscard]] static size_t char_pos_to_utf8_offset(const QString& text, int char_pos);
    [[nodiscard]] static int utf8_offset_to_char_pos(const QString& text, size_t utf8_offset);

    [[nodiscard]] QWidget* line_number_area_widget() const noexcept;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void paint_line_number_area(class QPaintEvent* event);

private:
    class LineNumberArea;

    void update_line_number_area_width(int new_block_count);
    void update_line_number_area(const QRect& rect, int dy);

    LineNumberArea*       line_number_area_ = nullptr;
    SqlSyntaxHighlighter* highlighter_      = nullptr;
    uint64_t              edit_revision_    = 0;
};

} // namespace capybaradb::ui
