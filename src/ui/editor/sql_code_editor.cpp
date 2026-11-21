#include "ui/editor/sql_code_editor.hpp"

#include "ui/editor/sql_syntax_highlighter.hpp"
#include "ui/shell/theme_tokens.hpp"

#include <QColor>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <algorithm>
#include <vector>

namespace capybaradb::ui {

class SqlCodeEditor::LineNumberArea final : public QWidget {
public:
    explicit LineNumberArea(SqlCodeEditor* editor)
        : QWidget(editor)
        , editor_(editor) {}

    [[nodiscard]] QSize sizeHint() const override {
        return QSize(editor_->line_number_area_width(), 0);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        editor_->paint_line_number_area(event);
    }

private:
    SqlCodeEditor* editor_;
};

SqlCodeEditor::SqlCodeEditor(QWidget* parent)
    : QPlainTextEdit(parent) {
    setObjectName(QStringLiteral("SqlCodeEditor"));
    setFont(theme::resolve_mono_font(theme::kEditorFontSizePx));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    document()->setDocumentMargin(6.0);

    line_number_area_ = new LineNumberArea(this);
    highlighter_      = new SqlSyntaxHighlighter(document());

    connect(this, &QPlainTextEdit::blockCountChanged, this, &SqlCodeEditor::update_line_number_area_width);
    connect(this, &QPlainTextEdit::updateRequest, this, &SqlCodeEditor::update_line_number_area);
    connect(this, &QPlainTextEdit::textChanged, this, [this]() { ++edit_revision_; });

    update_line_number_area_width(0);
}

int SqlCodeEditor::line_number_area_width() const {
    int digits = 1;
    int max_val = std::max(1, blockCount());
    while (max_val >= 10) {
        max_val /= 10;
        ++digits;
    }
    digits = std::max(digits, 2);
    const int space = 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    return space;
}

QWidget* SqlCodeEditor::line_number_area_widget() const noexcept {
    return line_number_area_;
}

void SqlCodeEditor::update_line_number_area_width(int) {
    setViewportMargins(line_number_area_width(), 0, 0, 0);
}

void SqlCodeEditor::update_line_number_area(const QRect& rect, int dy) {
    if (dy != 0) {
        line_number_area_->scroll(0, dy);
    } else {
        line_number_area_->update(0, rect.y(), line_number_area_->width(), rect.height());
    }
    if (rect.contains(viewport()->rect())) {
        update_line_number_area_width(0);
    }
}

void SqlCodeEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    line_number_area_->setGeometry(QRect(cr.left(), cr.top(), line_number_area_width(), cr.height()));
}

void SqlCodeEditor::paint_line_number_area(QPaintEvent* event) {
    QPainter painter(line_number_area_);
    painter.fillRect(event->rect(), QColor(theme::kPanelBg));
    painter.setPen(QColor(theme::kSplitterBg));
    painter.drawLine(
        line_number_area_->width() - 1,
        event->rect().top(),
        line_number_area_->width() - 1,
        event->rect().bottom());

    QTextBlock block = firstVisibleBlock();
    int block_number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    painter.setFont(font());
    const QColor num_color(theme::kMutedTextFg);
    const int text_h = fontMetrics().height();

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const QString number = QString::number(block_number + 1);
            painter.setPen(num_color);
            painter.drawText(
                0,
                top,
                line_number_area_->width() - 6,
                text_h,
                Qt::AlignRight | Qt::AlignVCenter,
                number);
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++block_number;
    }
}

void SqlCodeEditor::set_engine_dialect(drivers::EngineKind engine) {
    if (highlighter_ != nullptr) {
        highlighter_->set_engine_dialect(engine);
    }
}

void SqlCodeEditor::set_highlighting_enabled(bool enabled) {
    if (highlighter_ != nullptr) {
        highlighter_->set_highlighting_enabled(enabled);
    }
}

std::expected<int, QString> SqlCodeEditor::comment_selection(bool uncomment) {
    if (isReadOnly()) {
        return std::unexpected(QStringLiteral("The SQL editor is read-only."));
    }

    constexpr int kMaxLines = 10000;
    QTextCursor cur = textCursor();
    const bool has_selection = cur.hasSelection();
    const int sel_start = cur.selectionStart();
    const int sel_end   = cur.selectionEnd();
    const int orig_caret = cur.position();

    const QTextBlock first_block = document()->findBlock(sel_start);
    QTextBlock last_block = document()->findBlock(sel_end);
    int first_num = first_block.blockNumber();
    int last_num  = last_block.blockNumber();

    if (has_selection && sel_end == last_block.position() && last_num > first_num) {
        --last_num;
    }
    if (last_num - first_num + 1 > kMaxLines) {
        return std::unexpected(
            QStringLiteral("Select at most 10,000 lines to comment or uncomment at once."));
    }

    struct LineOp {
        int offset = 0;
        int remove = 0;
    };
    std::vector<LineOp> ops;
    ops.reserve(static_cast<size_t>(last_num - first_num + 1));

    for (int b = first_num; b <= last_num; ++b) {
        const QTextBlock blk = document()->findBlockByNumber(b);
        if (!blk.isValid()) {
            continue;
        }
        const QString text = blk.text();
        int idx = 0;
        while (idx < text.size() && (text[idx] == u' ' || text[idx] == u'\t')) {
            ++idx;
        }
        const int abs_pos = blk.position() + idx;
        if (!uncomment) {
            ops.push_back(LineOp{abs_pos, 0});
        } else if (idx + 1 < text.size() && text[idx] == u'-' && text[idx + 1] == u'-') {
            ops.push_back(LineOp{abs_pos, 2});
        }
    }

    if (ops.empty()) {
        return 0;
    }

    int updated_caret = orig_caret;
    QTextCursor edit(document());
    edit.beginEditBlock();
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
        edit.setPosition(it->offset);
        if (it->remove > 0) {
            edit.setPosition(it->offset + it->remove, QTextCursor::KeepAnchor);
            edit.removeSelectedText();
            if (!has_selection && it->offset < updated_caret) {
                updated_caret = std::max(it->offset, updated_caret - std::min(it->remove, updated_caret - it->offset));
            }
        } else {
            edit.insertText(QStringLiteral("--"));
            if (!has_selection && it->offset <= updated_caret) {
                updated_caret += 2;
            }
        }
    }
    edit.endEditBlock();

    QTextCursor final_cur(document());
    if (has_selection) {
        const QTextBlock b0 = document()->findBlockByNumber(first_num);
        const QTextBlock b1 = document()->findBlockByNumber(last_num);
        const int start_pos = b0.position();
        const int end_pos   = b1.position() + b1.length() - 1;
        final_cur.setPosition(start_pos);
        final_cur.setPosition(std::max(start_pos, end_pos), QTextCursor::KeepAnchor);
    } else {
        const int max_pos = std::max(0, document()->characterCount() - 1);
        final_cur.setPosition(std::clamp(updated_caret, 0, max_pos));
    }
    setTextCursor(final_cur);
    return static_cast<int>(ops.size());
}

bool SqlCodeEditor::navigate_to_line(int line_1based) {
    if (line_1based < 1 || line_1based > blockCount()) {
        return false;
    }
    const QTextBlock blk = document()->findBlockByNumber(line_1based - 1);
    if (!blk.isValid()) {
        return false;
    }
    QTextCursor cur(blk);
    setTextCursor(cur);
    centerCursor();
    setFocus(Qt::ShortcutFocusReason);
    return true;
}

bool SqlCodeEditor::navigate_to_char_offset(int char_offset_0based, int select_length) {
    const int max_pos = std::max(0, document()->characterCount() - 1);
    if (char_offset_0based < 0 || char_offset_0based > max_pos) {
        return false;
    }
    QTextCursor cur(document());
    cur.setPosition(char_offset_0based);
    if (select_length > 0) {
        cur.setPosition(std::min(max_pos, char_offset_0based + select_length), QTextCursor::KeepAnchor);
    }
    setTextCursor(cur);
    centerCursor();
    setFocus(Qt::ShortcutFocusReason);
    return true;
}

QPoint SqlCodeEditor::completion_popup_global_pos() const {
    const QRect cr = cursorRect();
    return viewport()->mapToGlobal(QPoint(cr.left(), cr.bottom() + 2));
}

void SqlCodeEditor::replace_text_range(int start_char_pos, int end_char_pos, const QString& replacement) {
    if (isReadOnly()) {
        return;
    }
    const int max_pos = std::max(0, document()->characterCount() - 1);
    const int s = std::clamp(start_char_pos, 0, max_pos);
    const int e = std::clamp(end_char_pos, s, max_pos);
    QTextCursor cur(document());
    cur.beginEditBlock();
    cur.setPosition(s);
    cur.setPosition(e, QTextCursor::KeepAnchor);
    cur.insertText(replacement);
    cur.endEditBlock();
    setTextCursor(cur);
}

size_t SqlCodeEditor::char_pos_to_utf8_offset(const QString& text, int char_pos) {
    const int clamped = std::clamp(char_pos, 0, static_cast<int>(text.size()));
    return static_cast<size_t>(QStringView(text).left(clamped).toUtf8().size());
}

int SqlCodeEditor::utf8_offset_to_char_pos(const QString& text, size_t utf8_offset) {
    const QByteArray utf8 = text.toUtf8();
    const size_t clamped = std::min(utf8_offset, static_cast<size_t>(utf8.size()));
    return static_cast<int>(QString::fromUtf8(utf8.constData(), static_cast<qsizetype>(clamped)).size());
}

} // namespace capybaradb::ui
