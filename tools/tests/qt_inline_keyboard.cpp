// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Frontend regression probe. Link against the built frontend objects, replacing
// main.cpp's object with this entry point. No guest, keys or firmware are used.
#include <iostream>
#include <stdexcept>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QTextEdit>
#include "core/core.h"
#include "yuzu/applets/qt_software_keyboard.h"

using namespace Service::AM::Frontend;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        Core::System system;
        Core::Frontend::KeyboardInitializeParameters init{};
        init.initial_text = u"AB";
        init.initial_cursor_position = 1;
        init.max_text_length = 8;
        init.type = SwkbdType::Normal;
        init.text_draw_type = SwkbdTextDrawType::Line;
        init.enable_backspace_button = true;
        init.enable_return_button = true;
        QtSoftwareKeyboardDialog dialog(nullptr, system, true, init);
        dialog.setAttribute(Qt::WA_DeleteOnClose, false);
        // This check also runs safely against the old frontend: its inline
        // initial text is never supplied to any display widget.
        auto* display = dialog.findChild<QTextEdit*>(QStringLiteral("inline_text_osk"));
        Check(display && display->toPlainText() == QStringLiteral("AB"),
              "inline initial text has no synchronized display");
        Check(display->textCursor().position() == 1, "initial cursor mismatch");
        int replies = 0;
        std::u16string reply_text;
        int reply_cursor = -1;
        SwkbdReplyType reply_kind{};
        QObject::connect(&dialog, &QtSoftwareKeyboardDialog::SubmitInlineText,
                         [&](SwkbdReplyType kind, std::u16string text, s32 cursor) {
                             ++replies;
                             reply_kind = kind;
                             reply_text = std::move(text);
                             reply_cursor = cursor;
                         });
        Core::Frontend::InlineAppearParameters appear{};
        appear.max_text_length = 8;
        appear.type = SwkbdType::Normal;
        appear.enable_backspace_button = true;
        appear.enable_return_button = true;
        dialog.ShowInlineKeyboard(appear, {}, {1280, 720});
        app.processEvents();
        Check(display->isVisible() && display->width() > 100 && display->height() > 20,
              "inline display hidden or collapsed");
        Check(display->isReadOnly() && display->focusPolicy() == Qt::NoFocus,
              "inline display must not own input");
        Check(replies == 0, "show emitted a guest reply");
        auto check_caret = [&] {
            const auto rect = display->cursorRect();
            Check(display->viewport()->rect().contains(rect.center()), "caret is offscreen");
            const auto image = display->viewport()->grab().toImage();
            const auto ratio = image.devicePixelRatio();
            Check(image.pixelColor(static_cast<int>(rect.left() * ratio),
                                   static_cast<int>(rect.center().y() * ratio)) ==
                      display->palette().color(QPalette::Text),
                  "unfocused read-only caret is not painted");
        };
        check_caret();
        auto key = [&](int code, const QString& text = {}) {
            QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier, text);
            QApplication::sendEvent(&dialog, &event);
            app.processEvents();
        };
        key(Qt::Key_C, QStringLiteral("C"));
        Check(display->toPlainText() == QStringLiteral("ACB") &&
                  display->textCursor().position() == 2 && replies == 1 &&
                  reply_text == u"ACB" && reply_cursor == 2 &&
                  reply_kind == SwkbdReplyType::ChangedString, "physical-key insertion mismatch");
        const auto caret_before_left = display->cursorRect().left();
        key(Qt::Key_Left);
        Check(display->textCursor().position() == 1 && replies == 2 &&
                  reply_kind == SwkbdReplyType::MovedCursor, "cursor movement mismatch");
        Check(display->cursorRect().left() < caret_before_left, "painted cursor did not move left");
        check_caret();
        key(Qt::Key_Backspace);
        Check(display->toPlainText() == QStringLiteral("CB") &&
                  display->textCursor().position() == 0 && replies == 3,
              "backspace display mismatch");
        key(Qt::Key_Backspace);
        key(Qt::Key_Left);
        Check(replies == 3, "left boundary emitted a spurious reply");
        dialog.InlineTextChanged({u"A\nB", 2});
        Check(display->toPlainText() == QStringLiteral("A\nB") &&
                  display->textCursor().position() == 2 && replies == 3,
              "guest text/cursor update echoed or failed to render");
        dialog.findChild<QPushButton*>(QStringLiteral("button_q"))->click();
        Check(display->toPlainText() == QStringLiteral("A\nqB") && replies == 4 &&
                  reply_text == u"A\nqB", "onscreen-button insertion mismatch");
        dialog.InlineTextChanged({u"12345678", 8});
        key(Qt::Key_X, QStringLiteral("x"));
        Check(display->toPlainText() == QStringLiteral("12345678") && replies == 4,
              "maximum-length rejection changed display or emitted reply");
        dialog.InlineTextChanged({u"A<&\n\u03a9\u4e2d", 4});
        app.processEvents();
        Check(display->toPlainText() == QString::fromStdU16String(u"A<&\n\u03a9\u4e2d"),
              "text was treated as markup");
        if (argc > 1) {
            Check(dialog.grab().save(QString::fromLocal8Bit(argv[1])), "screenshot failed");
        }
        dialog.HideInlineKeyboard();
        dialog.InlineTextChanged({u"42", 1});
        appear.type = SwkbdType::NumberPad;
        dialog.ShowInlineKeyboard(appear, {}, {1280, 720});
        app.processEvents();
        Check(display->isVisible() && display->toPlainText() == QStringLiteral("42") &&
                  display->textCursor().position() == 1 && replies == 4,
              "hide/update/numberpad reappear mismatch");
        dialog.findChild<QPushButton*>(QStringLiteral("button_3_num"))->click();
        Check(display->toPlainText() == QStringLiteral("432") && replies == 5 &&
                  reply_text == u"432", "numberpad insertion mismatch");
        dialog.InlineTextChanged({u"A\U0001f600B", 3});
        Check(display->toPlainText() == QString::fromStdU16String(u"A\U0001f600B") &&
                  display->textCursor().position() == 3, "UTF-16 surrogate-pair cursor mismatch");
        check_caret();
        dialog.InlineTextChanged({u"A\r\nB", 4});
        Check(display->toPlainText() == QStringLiteral("A\nB") &&
                  display->textCursor().position() == 3, "CRLF document cursor mismatch");
        dialog.reject();
        Check(replies == 6 && reply_kind == SwkbdReplyType::DecidedCancel,
              "cancel callback changed");
        dialog.HideInlineKeyboard();
        appear.max_text_length = 1000;
        std::u16string long_text;
        for (int i = 0; i < 30; ++i) long_text += u"0123456789\n";
        dialog.InlineTextChanged({long_text, static_cast<s32>(long_text.size())});
        dialog.ShowInlineKeyboard(appear, {}, {640, 360});
        app.processEvents();
        check_caret();
        dialog.HideInlineKeyboard();
        QtSoftwareKeyboardDialog normal(nullptr, system, false, init);
        normal.setAttribute(Qt::WA_DeleteOnClose, false);
        normal.ShowNormalKeyboard({}, {1280, 720});
        app.processEvents();
        auto* line = normal.findChild<QLineEdit*>(QStringLiteral("line_edit_osk"));
        Check(line && line->isVisible() && line->text() == QStringLiteral("AB") &&
                  !line->isReadOnly(), "normal keyboard changed");
        normal.hide();
        std::cout << "PASS: inline initial/show/edit/cursor/backspace/guest-update/limit/"
                     "literal-text/reappear/numberpad and normal-keyboard checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
