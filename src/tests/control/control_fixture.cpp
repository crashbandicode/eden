// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QShortcut>
#include <QSpinBox>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include "yuzu/control_server.h"

namespace {
// Records what the application observed when a command ran, so tests can check the exact
// Qt path (for example whether a popup menu had already closed when its action fired).
QLabel* status = nullptr;

void Note(const QString& text) {
    if (status) {
        status->setText(text);
    }
}

// A tree with a context menu; the same shape Eden's game list and input mapping buttons use:
// a stack-allocated QMenu that only exists while exec() runs.
QTreeWidget* ContextTree(QWidget* parent, const QString& name, const QString& prefix) {
    auto* tree = new QTreeWidget{parent};
    tree->setObjectName(name);
    tree->setColumnCount(2);
    auto* item = new QTreeWidgetItem{tree, {QStringLiteral("Test Game"), QStringLiteral("1.0")}};
    item->setCheckState(0, Qt::Unchecked);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(tree, &QTreeWidget::customContextMenuRequested, tree, [tree, prefix](const QPoint& position) {
        QMenu context_menu;
        context_menu.setObjectName(prefix + QStringLiteral("_menu"));
        QAction* configure = context_menu.addAction(QStringLiteral("Configure Game"));
        configure->setObjectName(prefix + QStringLiteral("_configure"));
        QObject::connect(configure, &QAction::triggered, tree, [&context_menu, prefix] {
            Note(prefix + QStringLiteral("_configure fired menu_visible=%1").arg(context_menu.isVisible() ? 1 : 0));
        });
        QAction* disabled = context_menu.addAction(QStringLiteral("Unavailable"));
        disabled->setObjectName(prefix + QStringLiteral("_disabled"));
        disabled->setEnabled(false);
        QObject::connect(disabled, &QAction::triggered, tree, [prefix] { Note(prefix + QStringLiteral("_disabled fired")); });
        context_menu.exec(tree->viewport()->mapToGlobal(position));
        Note(status->text() + QStringLiteral(" | menu closed"));
    });
    return tree;
}
} // namespace

int main(int argc, char** argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app{argc, argv};
    const auto args = app.arguments();
    const auto argument = [&](const QString& flag) {
        const int index = args.indexOf(flag);
        return index >= 0 && index + 1 < args.size() ? args[index + 1] : QString{};
    };
    ControlServer server;
    QString error;
    if (!server.Listen(argument(QStringLiteral("--control-socket")), argument(QStringLiteral("--control-log")), &error)) {
        qCritical("%s", qPrintable(error));
        return 2;
    }
    QMainWindow main;
    main.setObjectName(QStringLiteral("main"));
    main.setWindowTitle(QStringLiteral("Eden control test fixture"));
    auto* container = new QWidget{&main};
    auto* layout = new QVBoxLayout{container};
    auto* spin = new QSpinBox{container};
    spin->setObjectName(QStringLiteral("volume"));
    spin->setRange(0, 100);
    layout->addWidget(spin);
    auto* line = new QLineEdit{container};
    line->setObjectName(QStringLiteral("filename"));
    layout->addWidget(line);
    auto* combo = new QComboBox{container};
    combo->setObjectName(QStringLiteral("renderer"));
    combo->addItems({QStringLiteral("Vulkan"), QStringLiteral("OpenGL")});
    layout->addWidget(combo);
    auto* disabled = new QPushButton{QStringLiteral("Unavailable"), container};
    disabled->setObjectName(QStringLiteral("disabled"));
    disabled->setEnabled(false);
    layout->addWidget(disabled);
    layout->addWidget(ContextTree(container, QStringLiteral("games"), QStringLiteral("ctx")));
    status = new QLabel{QStringLiteral("idle"), container};
    status->setObjectName(QStringLiteral("status"));
    // A colour no theme paints, so screenshot tests can prove the pixels came from this window.
    status->setAutoFillBackground(true);
    status->setStyleSheet(QStringLiteral("QLabel { background-color: #ff00ff; color: #000000; }"));
    status->setMinimumHeight(32);
    layout->addWidget(status);
    main.setCentralWidget(container);

    auto* menu = main.menuBar()->addMenu(QStringLiteral("Test"));
    menu->setObjectName(QStringLiteral("test_menu"));
    auto* action = menu->addAction(QStringLiteral("Configure"));
    action->setObjectName(QStringLiteral("configure"));
    QObject::connect(action, &QAction::triggered, &main, [&main] {
        QDialog dialog{&main};
        dialog.setObjectName(QStringLiteral("configuration"));
        auto* dialog_layout = new QVBoxLayout{&dialog};
        auto* box = new QDialogButtonBox{QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog};
        box->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("accept"));
        dialog_layout->addWidget(new QLabel{QStringLiteral("A modal dialog controlled through the same endpoint"), &dialog});
        dialog_layout->addWidget(ContextTree(&dialog, QStringLiteral("dialog_games"), QStringLiteral("dlg")));
        dialog_layout->addWidget(box);
        QObject::connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        dialog.exec();
    });
    auto* checkable = menu->addAction(QStringLiteral("Fullscreen"));
    checkable->setObjectName(QStringLiteral("fullscreen"));
    checkable->setCheckable(true);
    QObject::connect(checkable, &QAction::triggered, &main, [](bool checked) { Note(QStringLiteral("fullscreen triggered=%1").arg(checked ? 1 : 0)); });

    // A second top-level window, like Eden's detached render window or multiplayer lobby.
    QWidget secondary;
    secondary.setObjectName(QStringLiteral("secondary"));
    auto* secondary_layout = new QVBoxLayout{&secondary};
    auto* secondary_button = new QPushButton{QStringLiteral("Secondary"), &secondary};
    secondary_button->setObjectName(QStringLiteral("secondary_button"));
    QObject::connect(secondary_button, &QPushButton::clicked, &secondary, [] { Note(QStringLiteral("secondary clicked")); });
    secondary_layout->addWidget(secondary_button);

    // Window-modal dialog on the main window, the modality Eden's applets use.
    auto* window_modal = menu->addAction(QStringLiteral("Window modal"));
    window_modal->setObjectName(QStringLiteral("window_modal"));
    QObject::connect(window_modal, &QAction::triggered, &main, [&main] {
        QDialog dialog{&main};
        dialog.setObjectName(QStringLiteral("window_modal_dialog"));
        dialog.setWindowModality(Qt::WindowModal);
        auto* dialog_layout = new QVBoxLayout{&dialog};
        auto* box = new QDialogButtonBox{QDialogButtonBox::Ok, &dialog};
        box->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("window_modal_accept"));
        dialog_layout->addWidget(box);
        QObject::connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        dialog.exec();
    });

    // Hotkey-only commands: a QShortcut without any QAction, as Eden's HotkeyRegistry creates.
    auto* bare_shortcut = new QShortcut{QKeySequence{QStringLiteral("F10")}, &main};
    bare_shortcut->setObjectName(QStringLiteral("bare_shortcut"));
    bare_shortcut->setContext(Qt::ApplicationShortcut);
    QObject::connect(bare_shortcut, &QShortcut::activated, &main, [] { Note(QStringLiteral("bare_shortcut fired")); });
    auto* disabled_shortcut = new QShortcut{QKeySequence{QStringLiteral("F11")}, &main};
    disabled_shortcut->setObjectName(QStringLiteral("disabled_shortcut"));
    disabled_shortcut->setEnabled(false);
    QObject::connect(disabled_shortcut, &QShortcut::activated, &main, [] { Note(QStringLiteral("disabled_shortcut fired")); });

    server.SetHandler([](const QString& method, const QJsonObject&) {
        if (method == QStringLiteral("status")) {
            return ControlServer::Success(QJsonObject{{QStringLiteral("fixture"), true}});
        }
        if (method == QStringLiteral("fixture.note")) {
            return ControlServer::Success(QJsonObject{{QStringLiteral("text"), status ? status->text() : QString{}}});
        }
        if (method == QStringLiteral("fixture.deactivate")) {
            // Simulates the operator's terminal holding focus; no widget is the active window.
            QT_WARNING_PUSH
            QT_WARNING_DISABLE_DEPRECATED
            QApplication::setActiveWindow(nullptr);
            QT_WARNING_POP
            return ControlServer::Success(QJsonObject{{QStringLiteral("active_window"), QApplication::activeWindow() != nullptr}});
        }
        return ControlServer::Error(QStringLiteral("unknown_method"), QStringLiteral("Unknown fixture command"));
    });
    main.resize(600, 400);
    main.show();
    secondary.resize(200, 100);
    secondary.show();
    if (args.contains(QStringLiteral("--test-hang"))) {
        QTimer::singleShot(1500, &app, [] { QThread::sleep(60); });
    }
    if (args.contains(QStringLiteral("--test-memory"))) {
        QTimer::singleShot(1500, &app, [] { QByteArray allocation{768 * 1024 * 1024, 'x'}; QThread::sleep(60); });
    }
    return app.exec();
}
