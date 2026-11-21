#pragma once

#include "dialects/sql_dialect.hpp"
#include "drivers/common/db_types.hpp"

#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <array>
#include <vector>

class QTextDocument;

namespace capybaradb::ui {

class SqlBlockUserData final : public QTextBlockUserData {
public:
    dialects::LexicalState state;
};

class SqlSyntaxHighlighter final : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit SqlSyntaxHighlighter(QTextDocument* parent = nullptr);

    void set_engine_dialect(drivers::EngineKind engine);
    [[nodiscard]] drivers::EngineKind engine_dialect() const noexcept { return engine_; }

    void set_highlighting_enabled(bool enabled);
    [[nodiscard]] bool is_highlighting_enabled() const noexcept { return enabled_; }

    [[nodiscard]] QTextCharFormat format_for_kind(dialects::LexicalKind kind) const;

protected:
    void highlightBlock(const QString& text) override;

private:
    void init_formats();

    drivers::EngineKind          engine_  = drivers::EngineKind::PostgreSql;
    bool                         enabled_ = true;
    std::array<QTextCharFormat, 10> formats_{};
    std::vector<dialects::TokenSpan> scratch_tokens_;
};

} // namespace capybaradb::ui
