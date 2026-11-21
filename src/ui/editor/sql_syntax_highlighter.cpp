#include "ui/editor/sql_syntax_highlighter.hpp"

#include "ui/shell/theme_tokens.hpp"

#include <QColor>
#include <QTextBlock>

namespace capybaradb::ui {

SqlSyntaxHighlighter::SqlSyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent) {
    init_formats();
}

void SqlSyntaxHighlighter::init_formats() {
    QTextCharFormat comment_fmt;
    comment_fmt.setForeground(QColor(theme::kSyntaxComment));

    QTextCharFormat string_fmt;
    string_fmt.setForeground(QColor(theme::kSyntaxString));

    QTextCharFormat keyword_fmt;
    keyword_fmt.setForeground(QColor(theme::kSyntaxKeyword));

    QTextCharFormat local_var_fmt;
    local_var_fmt.setForeground(QColor(theme::kSyntaxLocalVar));

    QTextCharFormat global_var_fmt;
    global_var_fmt.setForeground(QColor(theme::kSyntaxGlobalVar));

    QTextCharFormat bracket_fmt;
    bracket_fmt.setForeground(QColor(theme::kSyntaxBracketIdent));

    formats_[static_cast<size_t>(dialects::LexicalKind::Code)]              = {};
    formats_[static_cast<size_t>(dialects::LexicalKind::Comment)]           = comment_fmt;
    formats_[static_cast<size_t>(dialects::LexicalKind::String)]            = string_fmt;
    formats_[static_cast<size_t>(dialects::LexicalKind::EscapeString)]      = string_fmt;
    formats_[static_cast<size_t>(dialects::LexicalKind::QuotedIdentifier)]  = string_fmt;
    formats_[static_cast<size_t>(dialects::LexicalKind::BracketIdentifier)] = bracket_fmt;
    formats_[static_cast<size_t>(dialects::LexicalKind::DollarString)]      = string_fmt;
    formats_[static_cast<size_t>(dialects::LexicalKind::Keyword)]           = keyword_fmt;
    formats_[static_cast<size_t>(dialects::LexicalKind::LocalVariable)]     = local_var_fmt;
    formats_[static_cast<size_t>(dialects::LexicalKind::GlobalVariable)]    = global_var_fmt;
}

void SqlSyntaxHighlighter::set_engine_dialect(drivers::EngineKind engine) {
    if (engine_ == engine) {
        return;
    }
    engine_ = engine;
    rehighlight();
}

void SqlSyntaxHighlighter::set_highlighting_enabled(bool enabled) {
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    rehighlight();
}

QTextCharFormat SqlSyntaxHighlighter::format_for_kind(dialects::LexicalKind kind) const {
    const size_t idx = static_cast<size_t>(kind);
    return (idx < formats_.size()) ? formats_[idx] : QTextCharFormat{};
}

void SqlSyntaxHighlighter::highlightBlock(const QString& text) {
    if (!enabled_) {
        setCurrentBlockUserData(nullptr);
        setCurrentBlockState(0);
        return;
    }

    dialects::LexicalState initial_state;
    const QTextBlock prev = currentBlock().previous();
    if (prev.isValid()) {
        if (const auto* prev_data = static_cast<const SqlBlockUserData*>(prev.userData())) {
            initial_state = prev_data->state;
        }
    }

    const QByteArray utf8 = text.toUtf8();
    const dialects::LexicalState end_state = dialects::scan_sql_line(
        engine_,
        std::string_view(utf8.constData(), static_cast<size_t>(utf8.size())),
        initial_state,
        &scratch_tokens_);

    const bool pure_ascii = (utf8.size() == text.size());
    if (pure_ascii) {
        for (const auto& tok : scratch_tokens_) {
            if (tok.kind != dialects::LexicalKind::Code && tok.length > 0) {
                setFormat(
                    static_cast<int>(tok.start),
                    static_cast<int>(tok.length),
                    format_for_kind(tok.kind));
            }
        }
    } else if (!scratch_tokens_.empty()) {
        // Build byte -> UTF-16 code unit index map once for lines containing non-ASCII characters.
        std::vector<int> byte_to_u16(static_cast<size_t>(utf8.size()) + 1, 0);
        int u16_pos = 0;
        const size_t nbytes = static_cast<size_t>(utf8.size());
        size_t b = 0;
        while (b < nbytes) {
            byte_to_u16[b] = u16_pos;
            const unsigned char ch = static_cast<unsigned char>(utf8[static_cast<qsizetype>(b)]);
            size_t step = 1;
            int u16_inc = 1;
            if (ch < 0x80u) {
                step = 1;
                u16_inc = 1;
            } else if ((ch & 0xE0u) == 0xC0u) {
                step = 2;
                u16_inc = 1;
            } else if ((ch & 0xF0u) == 0xE0u) {
                step = 3;
                u16_inc = 1;
            } else if ((ch & 0xF8u) == 0xF0u) {
                step = 4;
                u16_inc = 2;
            }
            const size_t limit = std::min(nbytes, b + step);
            for (size_t k = b + 1; k < limit; ++k) {
                byte_to_u16[k] = u16_pos;
            }
            u16_pos += u16_inc;
            b = limit;
        }
        byte_to_u16[nbytes] = u16_pos;

        for (const auto& tok : scratch_tokens_) {
            if (tok.kind != dialects::LexicalKind::Code && tok.length > 0) {
                const size_t bs = std::min<size_t>(tok.start, nbytes);
                const size_t be = std::min<size_t>(static_cast<size_t>(tok.start) + tok.length, nbytes);
                const int us = byte_to_u16[bs];
                const int ue = byte_to_u16[be];
                if (ue > us) {
                    setFormat(us, ue - us, format_for_kind(tok.kind));
                }
            }
        }
    }

    if (end_state.is_default_code()) {
        setCurrentBlockUserData(nullptr);
        setCurrentBlockState(0);
    } else {
        auto* out_data = new SqlBlockUserData();
        out_data->state = end_state;
        setCurrentBlockUserData(out_data);
        setCurrentBlockState(end_state.to_block_state_int());
    }
}

} // namespace capybaradb::ui
