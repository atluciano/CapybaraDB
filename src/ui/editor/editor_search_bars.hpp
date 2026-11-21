#pragma once

#include <QFrame>
#include <QMetaObject>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace capybaradb::ui {

class SqlCodeEditor;

class GoToLineBar final : public QFrame {
    Q_OBJECT

public:
    explicit GoToLineBar(QWidget* parent = nullptr);

    void open_for_editor(SqlCodeEditor* editor);
    void close_bar();
    bool navigate();

    [[nodiscard]] QLineEdit* line_edit() const noexcept { return line_edit_; }
    [[nodiscard]] QPushButton* go_button() const noexcept { return go_btn_; }
    [[nodiscard]] QPushButton* close_button() const noexcept { return close_btn_; }
    [[nodiscard]] QLabel* feedback_label() const noexcept { return feedback_label_; }

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    SqlCodeEditor* editor_         = nullptr;
    QLineEdit*     line_edit_      = nullptr;
    QPushButton*   go_btn_         = nullptr;
    QPushButton*   close_btn_      = nullptr;
    QLabel*        feedback_label_ = nullptr;
};

class FindReplaceBar final : public QFrame {
    Q_OBJECT

public:
    explicit FindReplaceBar(QWidget* parent = nullptr);

    void open_find(SqlCodeEditor* editor);
    void open_replace(SqlCodeEditor* editor);
    void close_bar();

    [[nodiscard]] bool is_replace_mode() const noexcept { return replace_mode_; }

    bool find_next();
    void replace_one();
    void replace_all();

    [[nodiscard]] QLineEdit* find_edit() const noexcept { return find_edit_; }
    [[nodiscard]] QLineEdit* replace_edit() const noexcept { return replace_edit_; }
    [[nodiscard]] QCheckBox* match_case_check() const noexcept { return match_case_check_; }
    [[nodiscard]] QCheckBox* whole_words_check() const noexcept { return whole_words_check_; }
    [[nodiscard]] QCheckBox* selection_only_check() const noexcept { return selection_only_check_; }
    [[nodiscard]] QPushButton* use_selection_button() const noexcept { return use_selection_btn_; }
    [[nodiscard]] QPushButton* find_next_button() const noexcept { return find_next_btn_; }
    [[nodiscard]] QPushButton* replace_button() const noexcept { return replace_btn_; }
    [[nodiscard]] QPushButton* replace_all_button() const noexcept { return replace_all_btn_; }
    [[nodiscard]] QPushButton* close_button() const noexcept { return close_btn_; }
    [[nodiscard]] QLabel* feedback_label() const noexcept { return feedback_label_; }

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void set_editor(SqlCodeEditor* editor);
    void set_replace_mode(bool replace_mode);
    void capture_selection();
    void on_scope_toggled(bool checked);
    void on_document_changed();
    bool ready_for_search();
    int  range_start() const;
    int  range_end() const;
    int  find_match(const QString& text, const QString& needle, int start, int end) const;
    bool has_word_boundaries(const QString& text, int start, int length) const;

    SqlCodeEditor*          editor_               = nullptr;
    QMetaObject::Connection doc_conn_;
    bool                    replace_mode_         = false;
    bool                    internal_edit_        = false;
    bool                    scope_valid_          = false;
    int                     scope_start_          = 0;
    int                     scope_end_            = 0;

    QLabel*      replace_label_        = nullptr;
    QLineEdit*   find_edit_            = nullptr;
    QLineEdit*   replace_edit_         = nullptr;
    QCheckBox*   match_case_check_     = nullptr;
    QCheckBox*   whole_words_check_    = nullptr;
    QCheckBox*   selection_only_check_ = nullptr;
    QPushButton* use_selection_btn_    = nullptr;
    QPushButton* find_next_btn_        = nullptr;
    QPushButton* replace_btn_          = nullptr;
    QPushButton* replace_all_btn_      = nullptr;
    QPushButton* close_btn_            = nullptr;
    QLabel*      feedback_label_       = nullptr;
};

} // namespace capybaradb::ui
