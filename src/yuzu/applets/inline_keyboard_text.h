// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <QPainter>
#include <QTextDocument>
#include <QTextEdit>

// A passive view of the inline applet's text. The dialog continues to own input
// and guest notifications; focusing/editing a second text model would bypass it.
class InlineKeyboardText final : public QTextEdit {
public:
    explicit InlineKeyboardText(QWidget* parent) : QTextEdit(parent) {
        setObjectName(QStringLiteral("inline_text_osk"));
        setReadOnly(true);
        setFocusPolicy(Qt::NoFocus);
        setTextInteractionFlags(Qt::NoTextInteraction);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setStyleSheet(QStringLiteral("QTextEdit { background: palette(base); color: palette(text); }"));
        setCursorWidth(2);
    }

    void SetText(const QString& text, int position) {
        if (toPlainText() != text) {
            setPlainText(text);
        }
        auto cursor = textCursor();
        position = std::clamp(position, 0, static_cast<int>(text.size()));
        // QTextDocument represents a CRLF pair as one paragraph separator.
        position -= static_cast<int>(text.first(position).count(QStringLiteral("\r\n")));
        cursor.setPosition(std::min(position, document()->characterCount() - 1));
        setTextCursor(cursor);
        ensureCursorVisible();
        viewport()->update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QTextEdit::paintEvent(event);
        // Qt suppresses the native caret in an unfocused read-only editor. Keep
        // the guest insertion position visible even during controller navigation.
        QPainter painter(viewport());
        const auto rect = cursorRect();
        painter.fillRect(QRect(rect.left(), rect.top(), cursorWidth(), rect.height()),
                         palette().brush(QPalette::Text));
    }

    void resizeEvent(QResizeEvent* event) override {
        QTextEdit::resizeEvent(event);
        ensureCursorVisible();
    }
};
