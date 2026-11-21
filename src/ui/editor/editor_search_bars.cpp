#include "ui/editor/editor_search_bars.hpp"

#include "ui/editor/sql_code_editor.hpp"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>
#include <algorithm>

namespace capybaradb::ui {
namespace {

bool is_word_qchar(QChar ch) noexcept {
    if (ch == u'_' || ch == u'$') {
        return true;
    }
    return ch.isLetterOrNumber() ||
           ch.category() == QChar::Mark_NonSpacing ||
           ch.category() == QChar::Mark_SpacingCombining ||
           ch.category() == QChar::Punctuation_Connector;
}

} // namespace

// ===========================================================================
// GoToLineBar
// ===========================================================================

GoToLineBar::GoToLineBar(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("GoToLineBar"));
    setFrameShape(QFrame::NoFrame);
    setVisible(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);

    row->addWidget(new QLabel(QStringLiteral("Line:"), this));
    line_edit_ = new QLineEdit(this);
    line_edit_->setFixedWidth(110);
    line_edit_->setMaxLength(10);
    line_edit_->setAccessibleName(QStringLiteral("Go to SQL line"));
    row->addWidget(line_edit_);

    go_btn_ = new QPushButton(QStringLiteral("Go"), this);
    go_btn_->setAutoDefault(true);
    row->addWidget(go_btn_);

    close_btn_ = new QPushButton(QStringLiteral("Close"), this);
    close_btn_->setAutoDefault(false);
    row->addWidget(close_btn_);
    row->addStretch(1);

    root->addLayout(row);

    feedback_label_ = new QLabel(QStringLiteral("Navigation does not change SQL."), this);
    feedback_label_->setWordWrap(true);
    root->addWidget(feedback_label_);

    connect(go_btn_, &QPushButton::clicked, this, &GoToLineBar::navigate);
    connect(line_edit_, &QLineEdit::returnPressed, this, &GoToLineBar::navigate);
    connect(close_btn_, &QPushButton::clicked, this, &GoToLineBar::close_bar);
}

void GoToLineBar::open_for_editor(SqlCodeEditor* editor) {
    editor_ = editor;
    if (editor_ == nullptr) {
        return;
    }
    setVisible(true);
    const int cur_line = editor_->textCursor().blockNumber() + 1;
    line_edit_->setText(QString::number(cur_line));
    feedback_label_->setText(
        QStringLiteral("Line 1\u2013%1. Navigation does not change SQL.").arg(editor_->blockCount()));
    line_edit_->setFocus(Qt::ShortcutFocusReason);
    line_edit_->selectAll();
}

void GoToLineBar::close_bar() {
    setVisible(false);
    if (editor_ != nullptr) {
        editor_->setFocus(Qt::ShortcutFocusReason);
    }
}

bool GoToLineBar::navigate() {
    if (editor_ == nullptr) {
        return false;
    }
    bool ok = false;
    const int max_line = std::max(1, editor_->blockCount());
    const int target = line_edit_->text().trimmed().toInt(&ok);
    if (!ok || target < 1 || target > max_line) {
        feedback_label_->setText(QStringLiteral("Enter a line from 1 to %1.").arg(max_line));
        line_edit_->setFocus(Qt::OtherFocusReason);
        line_edit_->selectAll();
        return false;
    }
    editor_->navigate_to_line(target);
    close_bar();
    return true;
}

void GoToLineBar::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close_bar();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

// ===========================================================================
// FindReplaceBar
// ===========================================================================

FindReplaceBar::FindReplaceBar(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("FindReplaceBar"));
    setFrameShape(QFrame::NoFrame);
    setVisible(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(5);

    auto* grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    grid->setColumnMinimumWidth(0, 96);
    grid->setColumnStretch(1, 1);

    auto* find_lbl = new QLabel(QStringLiteral("&Find what:"), this);
    find_edit_ = new QLineEdit(this);
    find_edit_->setAccessibleName(QStringLiteral("Find what"));
    find_lbl->setBuddy(find_edit_);
    grid->addWidget(find_lbl, 0, 0);
    grid->addWidget(find_edit_, 0, 1);

    replace_label_ = new QLabel(QStringLiteral("&Replace with:"), this);
    replace_edit_ = new QLineEdit(this);
    replace_edit_->setAccessibleName(QStringLiteral("Replace with"));
    replace_label_->setBuddy(replace_edit_);
    grid->addWidget(replace_label_, 1, 0);
    grid->addWidget(replace_edit_, 1, 1);
    root->addLayout(grid);

    auto* opts_row = new QHBoxLayout();
    opts_row->setContentsMargins(0, 2, 0, 0);
    opts_row->setSpacing(12);
    match_case_check_ = new QCheckBox(QStringLiteral("Match &case"), this);
    match_case_check_->setChecked(true);
    opts_row->addWidget(match_case_check_);

    whole_words_check_ = new QCheckBox(QStringLiteral("Whole &words"), this);
    opts_row->addWidget(whole_words_check_);

    selection_only_check_ = new QCheckBox(QStringLiteral("&Selection only"), this);
    opts_row->addWidget(selection_only_check_);

    use_selection_btn_ = new QPushButton(QStringLiteral("&Use current selection"), this);
    use_selection_btn_->setEnabled(false);
    use_selection_btn_->setAutoDefault(false);
    opts_row->addWidget(use_selection_btn_);
    opts_row->addStretch(1);
    root->addLayout(opts_row);

    auto* btns_row = new QHBoxLayout();
    btns_row->setContentsMargins(0, 2, 0, 0);
    btns_row->setSpacing(6);
    find_next_btn_ = new QPushButton(QStringLiteral("Find &Next"), this);
    find_next_btn_->setAutoDefault(true);
    btns_row->addWidget(find_next_btn_);

    replace_btn_ = new QPushButton(QStringLiteral("Re&place"), this);
    replace_btn_->setAutoDefault(false);
    btns_row->addWidget(replace_btn_);

    replace_all_btn_ = new QPushButton(QStringLiteral("Replace &All"), this);
    replace_all_btn_->setAutoDefault(false);
    btns_row->addWidget(replace_all_btn_);

    close_btn_ = new QPushButton(QStringLiteral("Cl&ose"), this);
    close_btn_->setAutoDefault(false);
    btns_row->addWidget(close_btn_);
    btns_row->addStretch(1);
    root->addLayout(btns_row);

    feedback_label_ = new QLabel(
        QStringLiteral("Literal text in the current document. Changes do not execute SQL."),
        this);
    feedback_label_->setWordWrap(true);
    root->addWidget(feedback_label_);

    connect(selection_only_check_, &QCheckBox::toggled, this, &FindReplaceBar::on_scope_toggled);
    connect(use_selection_btn_, &QPushButton::clicked, this, &FindReplaceBar::capture_selection);
    connect(find_next_btn_, &QPushButton::clicked, this, &FindReplaceBar::find_next);
    connect(find_edit_, &QLineEdit::returnPressed, this, &FindReplaceBar::find_next);
    connect(replace_btn_, &QPushButton::clicked, this, &FindReplaceBar::replace_one);
    connect(replace_all_btn_, &QPushButton::clicked, this, &FindReplaceBar::replace_all);
    connect(close_btn_, &QPushButton::clicked, this, &FindReplaceBar::close_bar);
}

void FindReplaceBar::set_editor(SqlCodeEditor* editor) {
    if (editor_ == editor) {
        return;
    }
    if (doc_conn_) {
        disconnect(doc_conn_);
    }
    editor_ = editor;
    if (editor_ != nullptr) {
        doc_conn_ = connect(editor_, &QPlainTextEdit::textChanged, this, &FindReplaceBar::on_document_changed);
    }
}

void FindReplaceBar::set_replace_mode(bool replace_mode) {
    replace_mode_ = replace_mode;
    replace_label_->setVisible(replace_mode);
    replace_edit_->setVisible(replace_mode);
    replace_btn_->setVisible(replace_mode);
    replace_all_btn_->setVisible(replace_mode);
}

void FindReplaceBar::open_find(SqlCodeEditor* editor) {
    set_editor(editor);
    set_replace_mode(false);
    selection_only_check_->setChecked(false);
    setVisible(true);
    feedback_label_->setText(
        QStringLiteral("Literal text in the current document. Changes do not execute SQL."));
    find_edit_->setFocus(Qt::ShortcutFocusReason);
    find_edit_->selectAll();
}

void FindReplaceBar::open_replace(SqlCodeEditor* editor) {
    set_editor(editor);
    set_replace_mode(true);
    selection_only_check_->setChecked(false);
    setVisible(true);
    feedback_label_->setText(
        QStringLiteral("Literal text in the current document. Changes do not execute SQL."));
    find_edit_->setFocus(Qt::ShortcutFocusReason);
    find_edit_->selectAll();
}

void FindReplaceBar::close_bar() {
    setVisible(false);
    if (editor_ != nullptr) {
        editor_->setFocus(Qt::ShortcutFocusReason);
    }
}

void FindReplaceBar::on_scope_toggled(bool checked) {
    use_selection_btn_->setEnabled(checked);
    if (checked) {
        capture_selection();
    } else {
        scope_valid_ = false;
    }
}

void FindReplaceBar::capture_selection() {
    if (editor_ == nullptr) {
        scope_valid_ = false;
        return;
    }
    const QTextCursor cur = editor_->textCursor();
    if (!cur.hasSelection()) {
        scope_valid_ = false;
        feedback_label_->setText(QStringLiteral("Select text in the editor, then choose Use current selection."));
        return;
    }
    scope_start_ = cur.selectionStart();
    scope_end_   = cur.selectionEnd();
    scope_valid_ = true;
    feedback_label_->setText(QStringLiteral("Search/replace limited to captured selection."));
}

void FindReplaceBar::on_document_changed() {
    if (!internal_edit_ && selection_only_check_->isChecked()) {
        scope_valid_ = false;
        feedback_label_->setText(QStringLiteral("Document changed. Select text again and choose Use current selection."));
    }
}

bool FindReplaceBar::ready_for_search() {
    if (editor_ == nullptr) {
        return false;
    }
    if (selection_only_check_->isChecked() && !scope_valid_) {
        feedback_label_->setText(QStringLiteral("Select text in the editor, then choose Use current selection."));
        return false;
    }
    if (find_edit_->text().isEmpty()) {
        feedback_label_->setText(QStringLiteral("Enter text to find."));
        return false;
    }
    return true;
}

int FindReplaceBar::range_start() const {
    return selection_only_check_->isChecked() ? scope_start_ : 0;
}

int FindReplaceBar::range_end() const {
    if (selection_only_check_->isChecked()) {
        return scope_end_;
    }
    return editor_ != nullptr ? std::max(0, editor_->document()->characterCount() - 1) : 0;
}

bool FindReplaceBar::has_word_boundaries(const QString& text, int start, int length) const {
    if (!whole_words_check_->isChecked()) {
        return true;
    }
    const bool left_word = (start > 0 && start - 1 < text.size()) ? is_word_qchar(text[start - 1]) : false;
    const int right_idx = start + length;
    const bool right_word = (right_idx >= 0 && right_idx < text.size()) ? is_word_qchar(text[right_idx]) : false;
    return !left_word && !right_word;
}

int FindReplaceBar::find_match(const QString& text, const QString& needle, int start, int end) const {
    if (needle.isEmpty() || end > text.size()) {
        return -1;
    }
    const Qt::CaseSensitivity cs = match_case_check_->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;
    while (start <= end - needle.size()) {
        const int found = text.indexOf(needle, start, cs);
        if (found < 0 || found + needle.size() > end) {
            return -1;
        }
        if (has_word_boundaries(text, found, needle.size())) {
            return found;
        }
        start = found + 1;
    }
    return -1;
}

bool FindReplaceBar::find_next() {
    if (!ready_for_search()) {
        return false;
    }
    const QString text   = editor_->toPlainText();
    const QString needle = find_edit_->text();
    const int text_len   = static_cast<int>(text.size());
    const int r_start    = std::clamp(range_start(), 0, text_len);
    const int r_end      = std::clamp(range_end(), r_start, text_len);

    const int cur_end = editor_->textCursor().selectionEnd();
    const int start   = std::clamp(cur_end, r_start, r_end);
    int found = find_match(text, needle, start, r_end);
    bool wrapped = false;
    if (found < 0 && start > r_start) {
        found = find_match(text, needle, r_start, r_end);
        wrapped = true;
    }
    if (found < 0) {
        feedback_label_->setText(QStringLiteral("No matching text found."));
        return false;
    }

    editor_->navigate_to_char_offset(found, needle.size());
    feedback_label_->setText(
        wrapped ? QStringLiteral("Match selected. Search wrapped to the beginning.")
                : QStringLiteral("Match selected."));
    return true;
}

void FindReplaceBar::replace_one() {
    if (!ready_for_search()) {
        return;
    }
    if (editor_->isReadOnly()) {
        feedback_label_->setText(QStringLiteral("The document is read-only."));
        return;
    }
    const QString text   = editor_->toPlainText();
    const QString needle = find_edit_->text();
    const QString repl   = replace_edit_->text();
    const QTextCursor cur = editor_->textCursor();
    const int sel_start  = cur.selectionStart();
    const int sel_end    = cur.selectionEnd();
    const int text_len   = static_cast<int>(text.size());
    const int r_start    = std::clamp(range_start(), 0, text_len);
    const int r_end      = std::clamp(range_end(), r_start, text_len);
    const Qt::CaseSensitivity cs = match_case_check_->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive;

    if (!cur.hasSelection() ||
        sel_start < r_start ||
        sel_end > r_end ||
        QString::compare(cur.selectedText().replace(QChar::ParagraphSeparator, u'\n'), needle, cs) != 0 ||
        !has_word_boundaries(text, sel_start, sel_end - sel_start)) {
        find_next();
        return;
    }

    internal_edit_ = true;
    QTextCursor edit_cur = cur;
    edit_cur.beginEditBlock();
    edit_cur.insertText(repl);
    edit_cur.endEditBlock();
    internal_edit_ = false;

    if (selection_only_check_->isChecked()) {
        scope_end_ += repl.size() - needle.size();
    }
    editor_->setTextCursor(edit_cur);

    (void)find_next();
    feedback_label_->setText(QStringLiteral("Replaced 1 occurrence. ") + feedback_label_->text());
}

void FindReplaceBar::replace_all() {
    if (!ready_for_search()) {
        return;
    }
    if (editor_->isReadOnly()) {
        feedback_label_->setText(QStringLiteral("The document is read-only."));
        return;
    }

    const QString original = editor_->toPlainText();
    const QString needle   = find_edit_->text();
    const QString repl     = replace_edit_->text();
    const int orig_len     = static_cast<int>(original.size());
    const int from         = std::clamp(range_start(), 0, orig_len);
    const int until        = std::clamp(range_end(), from, orig_len);

    QString updated;
    updated.reserve(until - from);
    int position = from;
    int count    = 0;
    constexpr qsizetype kMaxChars = 16 * 1024 * 1024;

    while (true) {
        const int match = find_match(original, needle, position, until);
        const int end   = (match < 0) ? until : match;
        const qsizetype add_len = (end - position) + (match < 0 ? 0 : repl.size());
        if (updated.size() + add_len > kMaxChars) {
            feedback_label_->setText(QStringLiteral("Replacement exceeds the 16 million character limit. No changes made."));
            return;
        }
        updated.append(QStringView(original).mid(position, end - position));
        if (match < 0) {
            break;
        }
        updated.append(repl);
        ++count;
        position = match + needle.size();
    }

    if (count == 0) {
        feedback_label_->setText(QStringLiteral("No matching text found. No changes made."));
        return;
    }
    if (updated == QStringView(original).mid(from, until - from)) {
        feedback_label_->setText(QStringLiteral("Matching text is already identical to the replacement. No changes made."));
        return;
    }

    internal_edit_ = true;
    QTextCursor edit_cur(editor_->document());
    edit_cur.beginEditBlock();
    edit_cur.setPosition(from);
    edit_cur.setPosition(until, QTextCursor::KeepAnchor);
    edit_cur.insertText(updated);
    edit_cur.endEditBlock();
    internal_edit_ = false;

    if (selection_only_check_->isChecked()) {
        scope_end_ = from + updated.size();
    }
    editor_->navigate_to_char_offset(from, updated.size());
    feedback_label_->setText(
        QStringLiteral("Replaced %1 occurrence(s). Undo restores all changes together.").arg(count));
}

void FindReplaceBar::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        close_bar();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F3 && event->modifiers() == Qt::NoModifier) {
        find_next();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

} // namespace capybaradb::ui
