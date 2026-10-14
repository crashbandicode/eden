// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/macro_dialog.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonValue>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

#include "yuzu/macro_recorder.h"

namespace {

class LoadGuard {
public:
    explicit LoadGuard(bool& flag_) : flag{flag_} {
        flag = true;
    }
    ~LoadGuard() {
        flag = false;
    }
    LoadGuard(const LoadGuard&) = delete;
    LoadGuard& operator=(const LoadGuard&) = delete;

private:
    bool& flag;
};

bool BusyState(const QString& state) {
    return state == QStringLiteral("countdown") || state == QStringLiteral("recording") ||
           state == QStringLiteral("playing") || state == QStringLiteral("branch_playing");
}

QString NormalizePath(const QString& path) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty()) {
        return canonical;
    }
    return info.absoluteFilePath();
}

QString BindingText(const QJsonObject& row, const QString& top_key, const QString& nested_key) {
    const QString direct = row.value(top_key).toString();
    if (!direct.isEmpty()) {
        return direct;
    }
    return row.value(QStringLiteral("bindings")).toObject().value(nested_key).toString();
}

QString PlaybackText(const QString& keyboard, const QString& gamepad) {
    if (keyboard.isEmpty()) {
        return gamepad;
    }
    if (gamepad.isEmpty()) {
        return keyboard;
    }
    return QString(keyboard + QStringLiteral(" · ") + gamepad);
}

QString MarkerNameFrom(const QJsonObject& result) {
    const QJsonValue marker = result.value(QStringLiteral("marker"));
    if (marker.isObject()) {
        return marker.toObject().value(QStringLiteral("name")).toString();
    }
    if (marker.isString()) {
        return marker.toString();
    }
    return {};
}

QTableWidgetItem* TextItem(const QString& text) {
    auto* item = new QTableWidgetItem(text);
    auto flags = item->flags();
    flags.setFlag(Qt::ItemIsEditable, false);
    item->setFlags(flags);
    return item;
}

QJsonObject EntryFromInspect(const QJsonObject& inspected, const QString& fallback_path) {
    QJsonObject row;
    row.insert(QStringLiteral("name"), inspected.value(QStringLiteral("name")));
    QString path = NormalizePath(inspected.value(QStringLiteral("path")).toString());
    if (path.isEmpty()) {
        path = fallback_path;
    }
    row.insert(QStringLiteral("path"), path);
    row.insert(QStringLiteral("game_id"), inspected.value(QStringLiteral("game_id")));
    row.insert(QStringLiteral("player"), inspected.value(QStringLiteral("player")));
    row.insert(QStringLiteral("duration_frames"), inspected.value(QStringLiteral("duration_frames")));
    row.insert(QStringLiteral("markers"), inspected.value(QStringLiteral("markers")));
    const QJsonObject bindings = inspected.value(QStringLiteral("bindings")).toObject();
    QString keyboard = inspected.value(QStringLiteral("keyboard_shortcut")).toString();
    if (keyboard.isEmpty()) {
        keyboard = bindings.value(QStringLiteral("keyboard")).toString();
    }
    QString gamepad = inspected.value(QStringLiteral("gamepad_shortcut")).toString();
    if (gamepad.isEmpty()) {
        gamepad = bindings.value(QStringLiteral("gamepad")).toString();
    }
    row.insert(QStringLiteral("keyboard_shortcut"), keyboard);
    row.insert(QStringLiteral("gamepad_shortcut"), gamepad);
    return row;
}

} // namespace

MacroDialog::MacroDialog(MacroRecorder& recorder_, QWidget* parent, QWidget* game_window_)
    : QDialog(parent), recorder{recorder_}, game_window{game_window_ ? game_window_ : parent} {
    setWindowTitle(tr("Input Macros"));
    setObjectName(QStringLiteral("MacroRecorderDialog"));
    setModal(false);
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_QuitOnClose, false);
    setMinimumSize(640, 520);

    BuildUi();
    connect(&recorder, &MacroRecorder::Changed, this, &MacroDialog::OnRecorderChanged);
    ApplyStatus(recorder.Status());
    resize(880, 760);
}

void MacroDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!pending_error.isEmpty()) {
        const QString message = pending_error;
        pending_error.clear();
        QMessageBox::warning(this, tr("Input Macros"), message);
    }
    if (!BusyState(applied_state)) {
        ReloadShortcuts();
        ReloadLibrary();
    }
    if (!pending_error.isEmpty()) {
        const QString message = pending_error;
        pending_error.clear();
        QMessageBox::warning(this, tr("Input Macros"), message);
    }
}

void MacroDialog::BuildUi() {
    auto* outer = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("macroTabs"));
    auto* recording_page = new QWidget(tabs);
    auto* shortcut_page = new QWidget(tabs);
    auto* layout = new QVBoxLayout(recording_page);
    auto* shortcut_layout = new QVBoxLayout(shortcut_page);
    tabs->addTab(recording_page, tr("Record and play"));
    tabs->addTab(shortcut_page, tr("Shortcuts"));
    outer->addWidget(tabs);

    help_label = new QLabel(this);
    help_label->setObjectName(QStringLiteral("macroHelpLabel"));
    help_label->setWordWrap(true);
    help_label->setText(tr(
        "Start from the same game or menu state as the original recording. "
        "Record from marker replays the prefix, then records a new branch without changing the original. "
        "Recording starts after a three-second countdown."));

    status_label = new QLabel(tr("Ready."), this);
    status_label->setObjectName(QStringLiteral("macroStatusLabel"));
    status_label->setWordWrap(true);
    status_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont status_font = status_label->font();
    status_font.setBold(true);
    if (status_font.pointSizeF() > 0.0) {
        status_font.setPointSizeF(status_font.pointSizeF() + 2.0);
    }
    status_label->setFont(status_font);

    auto* tools = new QHBoxLayout;
    refresh_button = new QPushButton(tr("Refresh"), this);
    refresh_button->setObjectName(QStringLiteral("macroRefreshButton"));
    open_file_button = new QPushButton(tr("Open file…"), this);
    open_file_button->setObjectName(QStringLiteral("macroOpenFileButton"));
    open_folder_button = new QPushButton(tr("Open folder"), this);
    open_folder_button->setObjectName(QStringLiteral("macroOpenFolderButton"));
    tools->addWidget(refresh_button);
    tools->addWidget(open_file_button);
    tools->addWidget(open_folder_button);
    tools->addStretch(1);

    library = new QTableWidget(0, 4, this);
    library->setObjectName(QStringLiteral("macroLibrary"));
    QStringList headers;
    headers.append(tr("Name"));
    headers.append(tr("Frames"));
    headers.append(tr("Markers"));
    headers.append(tr("Playback shortcut"));
    library->setHorizontalHeaderLabels(headers);
    library->setSelectionBehavior(QAbstractItemView::SelectRows);
    library->setSelectionMode(QAbstractItemView::SingleSelection);
    library->setEditTriggers(QAbstractItemView::NoEditTriggers);
    library->setAlternatingRowColors(true);
    library->setShowGrid(false);
    library->verticalHeader()->setVisible(false);
    library->horizontalHeader()->setStretchLastSection(false);
    library->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    library->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    library->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    library->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    library->setMinimumHeight(160);
    library->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    marker_combo = new QComboBox(this);
    marker_combo->setObjectName(QStringLiteral("macroMarkerCombo"));
    marker_name_edit = new QLineEdit(this);
    marker_name_edit->setObjectName(QStringLiteral("macroMarkerNameEdit"));
    marker_name_edit->setPlaceholderText(tr("Leave blank for an automatic name"));
    name_edit = new QLineEdit(this);
    name_edit->setObjectName(QStringLiteral("macroNameEdit"));
    name_edit->setPlaceholderText(tr("Name for the new recording or branch"));
    player_combo = new QComboBox(this);
    player_combo->setObjectName(QStringLiteral("macroPlayerCombo"));
    for (int player = 0; player < 8; ++player) {
        player_combo->addItem(tr("Player %1").arg(player + 1), QVariant{player});
    }
    player_combo->addItem(tr("Handheld"), QVariant{8});
    player_combo->addItem(tr("Other"), QVariant{9});

    save_path_label = new QLabel(this);
    save_path_label->setObjectName(QStringLiteral("macroSavePathLabel"));
    save_path_label->setWordWrap(true);
    choose_save_button = new QPushButton(tr("Choose save file…"), this);
    choose_save_button->setObjectName(QStringLiteral("macroChooseSaveButton"));
    default_save_button = new QPushButton(tr("Use macros folder"), this);
    default_save_button->setObjectName(QStringLiteral("macroDefaultSaveButton"));
    auto* save_buttons = new QHBoxLayout;
    save_buttons->addWidget(choose_save_button);
    save_buttons->addWidget(default_save_button);
    save_buttons->addStretch(1);
    UpdateSavePathLabel();

    form->addRow(tr("Marker"), marker_combo);
    form->addRow(tr("Marker name"), marker_name_edit);
    form->addRow(tr("New name"), name_edit);
    form->addRow(tr("Player"), player_combo);
    form->addRow(tr("Save as"), save_buttons);
    form->addRow(save_path_label);

    auto* actions = new QHBoxLayout;
    record_button = new QPushButton(tr("Record new"), this);
    record_button->setObjectName(QStringLiteral("macroRecordButton"));
    record_button->setToolTip(tr("Record a new macro after a three-second countdown."));
    play_button = new QPushButton(tr("Play"), this);
    play_button->setObjectName(QStringLiteral("macroPlayButton"));
    play_button->setToolTip(tr("Play the selected macro. The game needs to be at the same starting point."));
    branch_button = new QPushButton(tr("Record from marker"), this);
    branch_button->setObjectName(QStringLiteral("macroBranchButton"));
    branch_button->setToolTip(
        tr("Play up to the selected marker, then record into a new file. The original is kept."));
    stop_button = new QPushButton(tr("Stop"), this);
    stop_button->setObjectName(QStringLiteral("macroStopButton"));
    stop_button->setToolTip(tr("Stop recording or playback."));
    add_marker_button = new QPushButton(tr("Add marker"), this);
    add_marker_button->setObjectName(QStringLiteral("macroAddMarkerButton"));
    add_marker_button->setToolTip(tr("Drop a marker at the current frame."));
    actions->addWidget(record_button);
    actions->addWidget(play_button);
    actions->addWidget(branch_button);
    actions->addWidget(stop_button);
    actions->addWidget(add_marker_button);
    actions->addStretch(1);

    keyboard_binding = new QKeySequenceEdit(this);
    keyboard_binding->setObjectName(QStringLiteral("macroKeyboardBinding"));
    keyboard_binding->setMinimumWidth(180);
    gamepad_binding = new QLineEdit(this);
    gamepad_binding->setObjectName(QStringLiteral("macroGamepadBinding"));
    gamepad_binding->setPlaceholderText(tr("l+r+minus"));
    save_binding_button = new QPushButton(tr("Save binding"), this);
    save_binding_button->setObjectName(QStringLiteral("macroSaveBindingButton"));
    auto* playback = new QGroupBox(tr("Playback shortcut"), this);
    auto* playback_form = new QFormLayout(playback);
    playback_form->addRow(tr("Keyboard"), keyboard_binding);
    playback_form->addRow(tr("Gamepad"), gamepad_binding);
    playback_form->addRow(save_binding_button);

    marker_keyboard = new QKeySequenceEdit(this);
    marker_keyboard->setObjectName(QStringLiteral("macroMarkerKeyboard"));
    marker_keyboard->setMinimumWidth(180);
    marker_keyboard->setKeySequence(
        QKeySequence::fromString(QStringLiteral("Ctrl+Shift+M"), QKeySequence::PortableText));
    marker_gamepad = new QLineEdit(this);
    marker_gamepad->setObjectName(QStringLiteral("macroMarkerGamepad"));
    marker_gamepad->setPlaceholderText(tr("l+r+minus"));
    gamepad_hint = new QLabel(this);
    gamepad_hint->setObjectName(QStringLiteral("macroGamepadHintLabel"));
    gamepad_hint->setWordWrap(true);
    gamepad_hint->setText(tr(
        "While you are recording, this shortcut drops a marker. Use the keyboard, the gamepad, or both. "
        "Gamepad shortcuts use button names joined by +, such as l+r+minus. "
        "Names: a, b, x, y, lstick, rstick, l, r, zl, zr, plus, minus, left, up, right, down, "
        "sl_left, sr_left, home, capture, sl_right, sr_right."));
    save_marker_shortcuts_button = new QPushButton(tr("Save marker shortcuts"), this);
    save_marker_shortcuts_button->setObjectName(QStringLiteral("macroSaveMarkerShortcutsButton"));
    stop_hint = new QLabel(tr("Stop from the keyboard with Ctrl+Shift+F12."), this);
    stop_hint->setObjectName(QStringLiteral("macroStopHintLabel"));
    stop_hint->setWordWrap(true);
    auto* marker_box = new QGroupBox(tr("Marker shortcut"), this);
    auto* marker_layout = new QVBoxLayout(marker_box);
    auto* marker_form = new QFormLayout;
    marker_form->addRow(tr("Keyboard"), marker_keyboard);
    marker_form->addRow(tr("Gamepad"), marker_gamepad);
    marker_layout->addLayout(marker_form);
    marker_layout->addWidget(gamepad_hint);
    marker_layout->addWidget(save_marker_shortcuts_button, 0, Qt::AlignLeft);
    marker_layout->addWidget(stop_hint);

    layout->addWidget(help_label);
    layout->addWidget(status_label);
    layout->addLayout(tools);
    layout->addWidget(library, 1);
    layout->addLayout(form);
    layout->addLayout(actions);
    shortcut_layout->addWidget(playback);
    shortcut_layout->addWidget(marker_box);
    shortcut_layout->addStretch(1);

    connect(refresh_button, &QPushButton::clicked, this, &MacroDialog::Refresh);
    connect(open_file_button, &QPushButton::clicked, this, &MacroDialog::OpenFile);
    connect(open_folder_button, &QPushButton::clicked, this, &MacroDialog::OpenFolder);
    connect(choose_save_button, &QPushButton::clicked, this, &MacroDialog::ChooseSavePath);
    connect(default_save_button, &QPushButton::clicked, this, &MacroDialog::UseDefaultSavePath);
    connect(record_button, &QPushButton::clicked, this, &MacroDialog::RecordNew);
    connect(play_button, &QPushButton::clicked, this, &MacroDialog::PlaySelected);
    connect(branch_button, &QPushButton::clicked, this, &MacroDialog::RecordFromMarker);
    connect(stop_button, &QPushButton::clicked, this, &MacroDialog::Stop);
    connect(add_marker_button, &QPushButton::clicked, this, &MacroDialog::AddMarker);
    connect(save_binding_button, &QPushButton::clicked, this, &MacroDialog::SaveBinding);
    connect(save_marker_shortcuts_button, &QPushButton::clicked, this, &MacroDialog::SaveMarkerShortcuts);
    connect(library, &QTableWidget::itemSelectionChanged, this, &MacroDialog::LoadSelectionIntoEditors);
}

void MacroDialog::OnRecorderChanged() {
    // Status arrives many times a second. Refresh the label and buttons only.
    ApplyStatus(recorder.Status());
}

void MacroDialog::ApplyStatus(const QJsonObject& status) {
    const QString state = status.value(QStringLiteral("state")).toString();
    const bool was_busy = BusyState(applied_state);
    const bool now_busy = BusyState(state);

    if (state != QStringLiteral("recording")) {
        recent_marker.clear();
    }
    status_label->setText(Describe(status));
    applied_state = state;
    UpdateActions();

    if (state == QStringLiteral("error")) {
        const QString error = status.value(QStringLiteral("error")).toString();
        if (!error.isEmpty()) {
            const bool show = inside_call == 0 && !error_posted;
            error_posted = true;
            if (show) {
                ShowError(error);
            }
        }
    } else {
        error_posted = false;
    }

    if (was_busy && !now_busy && !loading) {
        ReloadLibrary(NormalizePath(status.value(QStringLiteral("path")).toString()));
    }
}

QString MacroDialog::Describe(const QJsonObject& status) const {
    const QString state = status.value(QStringLiteral("state")).toString();
    const QString name = status.value(QStringLiteral("name")).toString();
    const QString frame = QString::number(status.value(QStringLiteral("frame")).toInt());
    const QString duration = QString::number(status.value(QStringLiteral("duration_frames")).toInt());
    QString text;
    if (state == QStringLiteral("countdown")) {
        const int remaining = status.value(QStringLiteral("countdown_remaining")).toInt();
        const int seconds = remaining > 0 ? (remaining + 999) / 1000 : 0;
        if (seconds > 0) {
            text = tr("Starting in %1. Click the game.").arg(seconds);
        } else {
            text = tr("Starting. Click the game.");
        }
    } else if (state == QStringLiteral("recording")) {
        if (recent_marker.isEmpty()) {
            text = tr("Recording \"%1\". Frame %2.").arg(name, frame);
        } else {
            text = tr("Recording \"%1\". Frame %2. Marker \"%3\" added.").arg(name, frame, recent_marker);
        }
    } else if (state == QStringLiteral("playing")) {
        text = tr("Playing \"%1\". Frame %2 of %3.").arg(name, frame, duration);
    } else if (state == QStringLiteral("branch_playing")) {
        text = tr("Playing up to the marker, then recording \"%1\". Frame %2 of %3.")
                   .arg(name, frame, duration);
    } else if (state == QStringLiteral("finished")) {
        text = tr("Finished \"%1\".").arg(name);
    } else if (state == QStringLiteral("error")) {
        text = status.value(QStringLiteral("error")).toString();
        if (text.isEmpty()) {
            text = tr("Something went wrong.");
        }
    } else {
        text = tr("Ready.");
    }
    if (status.value(QStringLiteral("paused")).toBool()) {
        text = tr("%1 Paused.").arg(text);
    }
    return text;
}

void MacroDialog::UpdateActions() {
    const bool busy = BusyState(applied_state);
    const bool recording = applied_state == QStringLiteral("recording");
    const bool has_row = !SelectedPath().isEmpty();
    const bool has_marker = !marker_combo->currentData().toString().isEmpty();

    record_button->setEnabled(!busy);
    play_button->setEnabled(!busy && has_row);
    branch_button->setEnabled(!busy && has_row && has_marker);
    stop_button->setEnabled(busy);
    add_marker_button->setEnabled(recording);

    refresh_button->setEnabled(!busy);
    open_file_button->setEnabled(!busy);
    open_folder_button->setEnabled(!busy);
    choose_save_button->setEnabled(!busy);
    default_save_button->setEnabled(!busy && !save_path.isEmpty());
    save_binding_button->setEnabled(!busy && has_row);
    save_marker_shortcuts_button->setEnabled(!busy);

    library->setEnabled(!busy);
    marker_combo->setEnabled(!busy && marker_combo->count() > 0);
    player_combo->setEnabled(!busy);
    keyboard_binding->setEnabled(!busy && has_row);
    gamepad_binding->setEnabled(!busy && has_row);
    marker_keyboard->setEnabled(!busy);
    marker_gamepad->setEnabled(!busy);
}

void MacroDialog::ReloadLibrary(const QString& preferred_path) {
    if (loading) {
        return;
    }
    const LoadGuard guard{loading};
    const QString keep = preferred_path.isEmpty() ? SelectedPath() : preferred_path;

    QJsonObject listed;
    if (!Call(QStringLiteral("macro.list"), QJsonObject{}, listed)) {
        UpdateActions();
        return;
    }

    macros_directory = listed.value(QStringLiteral("directory")).toString();
    const QJsonArray macros = listed.value(QStringLiteral("macros")).toArray();
    QHash<QString, QJsonObject> entries;
    QStringList order;
    for (qsizetype i = 0; i < macros.size(); ++i) {
        if (!macros.at(i).isObject()) {
            continue;
        }
        QJsonObject row = macros.at(i).toObject();
        const QString path = NormalizePath(row.value(QStringLiteral("path")).toString());
        if (path.isEmpty() || entries.contains(path)) {
            continue;
        }
        row.insert(QStringLiteral("path"), path);
        entries.insert(path, row);
        order.append(path);
    }

    QStringList still_external;
    for (const QString& extra : session_paths) {
        const QString path = NormalizePath(extra);
        if (path.isEmpty() || entries.contains(path)) {
            continue;
        }
        QJsonObject params;
        params.insert(QStringLiteral("path"), path);
        QJsonObject inspected;
        if (!Call(QStringLiteral("macro.inspect"), params, inspected)) {
            continue;
        }
        const QJsonObject row = EntryFromInspect(inspected, path);
        const QString row_path = row.value(QStringLiteral("path")).toString();
        if (row_path.isEmpty() || entries.contains(row_path)) {
            continue;
        }
        entries.insert(row_path, row);
        order.append(row_path);
        still_external.append(row_path);
    }
    session_paths = still_external;
    library_rows = entries;

    library->blockSignals(true);
    library->setRowCount(static_cast<int>(order.size()));
    int selected_row = -1;
    for (int row = 0; row < static_cast<int>(order.size()); ++row) {
        const QString path = order.at(row);
        const QJsonObject entry = entries.value(path);
        auto* name_item = TextItem(entry.value(QStringLiteral("name")).toString());
        name_item->setData(Qt::UserRole, path);
        library->setItem(row, 0, name_item);

        auto* frames_item =
            TextItem(QString::number(entry.value(QStringLiteral("duration_frames")).toInt()));
        frames_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        library->setItem(row, 1, frames_item);

        const QJsonArray markers = entry.value(QStringLiteral("markers")).toArray();
        auto* markers_item = TextItem(QString::number(static_cast<int>(markers.size())));
        markers_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        QStringList marker_names;
        for (qsizetype marker_index = 0; marker_index < markers.size(); ++marker_index) {
            const QString marker_name =
                markers.at(marker_index).toObject().value(QStringLiteral("name")).toString();
            if (!marker_name.isEmpty()) {
                marker_names.append(marker_name);
            }
        }
        if (!marker_names.isEmpty()) {
            markers_item->setToolTip(marker_names.join(QStringLiteral(", ")));
        }
        library->setItem(row, 2, markers_item);

        const QString shortcut = PlaybackText(
            BindingText(entry, QStringLiteral("keyboard_shortcut"), QStringLiteral("keyboard")),
            BindingText(entry, QStringLiteral("gamepad_shortcut"), QStringLiteral("gamepad")));
        library->setItem(row, 3, TextItem(shortcut));

        if (path == keep) {
            selected_row = row;
        }
    }
    if (selected_row >= 0) {
        library->setCurrentCell(selected_row, 0);
    } else {
        library->clearSelection();
    }
    library->blockSignals(false);
    LoadSelectionIntoEditors();
    ShowListErrors(listed.value(QStringLiteral("errors")).toArray());
    UpdateActions();
}

void MacroDialog::ReloadShortcuts() {
    QJsonObject result;
    if (!Call(QStringLiteral("macro.shortcuts"), QJsonObject{}, result)) {
        return;
    }
    ApplyShortcutResult(result);
}

void MacroDialog::ApplyShortcutResult(const QJsonObject& result) {
    if (result.contains(QStringLiteral("marker_keyboard"))) {
        marker_keyboard->setKeySequence(QKeySequence::fromString(
            result.value(QStringLiteral("marker_keyboard")).toString(), QKeySequence::PortableText));
    }
    if (result.contains(QStringLiteral("marker_gamepad"))) {
        marker_gamepad->setText(result.value(QStringLiteral("marker_gamepad")).toString());
    }
    const QString stop_keys = result.value(QStringLiteral("stop_keyboard")).toString();
    if (!stop_keys.isEmpty()) {
        stop_hint->setText(tr("Stop from the keyboard with %1.").arg(stop_keys));
    }
}

void MacroDialog::LoadSelectionIntoEditors() {
    const QJsonObject row = SelectedEntry();
    const QString previous_marker = marker_combo->currentData().toString();
    marker_combo->blockSignals(true);
    marker_combo->clear();
    const QJsonArray markers = row.value(QStringLiteral("markers")).toArray();
    for (qsizetype i = 0; i < markers.size(); ++i) {
        const QJsonObject marker = markers.at(i).toObject();
        const QString marker_name = marker.value(QStringLiteral("name")).toString();
        if (marker_name.isEmpty()) {
            continue;
        }
        const int frame = marker.value(QStringLiteral("frame")).toInt();
        marker_combo->addItem(tr("%1 (frame %2)").arg(marker_name, QString::number(frame)), marker_name);
    }
    if (marker_combo->count() > 0) {
        const int index = marker_combo->findData(previous_marker);
        marker_combo->setCurrentIndex(index >= 0 ? index : 0);
    }
    marker_combo->blockSignals(false);

    if (row.isEmpty()) {
        keyboard_binding->setKeySequence(QKeySequence{});
        gamepad_binding->clear();
    } else {
        keyboard_binding->setKeySequence(QKeySequence::fromString(
            BindingText(row, QStringLiteral("keyboard_shortcut"), QStringLiteral("keyboard")),
            QKeySequence::PortableText));
        gamepad_binding->setText(
            BindingText(row, QStringLiteral("gamepad_shortcut"), QStringLiteral("gamepad")));
        const int player_index =
            player_combo->findData(QVariant{row.value(QStringLiteral("player")).toInt()});
        if (player_index >= 0) {
            player_combo->setCurrentIndex(player_index);
        }
    }
    UpdateActions();
}

void MacroDialog::UpdateSavePathLabel() {
    if (save_path.isEmpty()) {
        save_path_label->setText(tr("New recordings are saved by name in the macros folder."));
        return;
    }
    save_path_label->setText(tr("Next new recording will be saved to %1.").arg(save_path));
}

void MacroDialog::YieldToGame() {
    hide();
    QWidget* host = game_window;
    if (host == nullptr) {
        return;
    }
    host->raise();
    host->activateWindow();
    host->setFocus(Qt::ActiveWindowFocusReason);
}

void MacroDialog::Refresh() {
    ReloadShortcuts();
    ReloadLibrary();
}

void MacroDialog::OpenFile() {
    const QString chosen = QFileDialog::getOpenFileName(
        this, tr("Open macro"), macros_directory, tr("Input macros (*.json);;All files (*)"), nullptr,
        QFileDialog::DontUseNativeDialog);
    if (chosen.isEmpty()) {
        return;
    }
    const QString path = NormalizePath(chosen);
    if (path.isEmpty()) {
        ShowError(tr("That file could not be opened."));
        return;
    }
    if (!session_paths.contains(path)) {
        session_paths.append(path);
    }
    ReloadLibrary(path);
}

void MacroDialog::OpenFolder() {
    if (macros_directory.isEmpty()) {
        ReloadLibrary();
    }
    if (macros_directory.isEmpty()) {
        ShowError(tr("The macros folder is not available yet."));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(macros_directory))) {
        ShowError(tr("The macros folder could not be opened."));
    }
}

void MacroDialog::ChooseSavePath() {
    QString suggested = macros_directory;
    const QString name = name_edit->text().trimmed();
    if (!name.isEmpty()) {
        const QString file_name = name.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)
                                      ? name
                                      : QString(name + QStringLiteral(".json"));
        if (macros_directory.isEmpty()) {
            suggested = file_name;
        } else {
            suggested = QDir(macros_directory).filePath(file_name);
        }
    }
    const QString chosen = QFileDialog::getSaveFileName(
        this, tr("Save macro as"), suggested, tr("Input macros (*.json);;All files (*)"), nullptr,
        QFileDialog::DontUseNativeDialog);
    if (chosen.isEmpty()) {
        return;
    }
    save_path = chosen;
    UpdateSavePathLabel();
    UpdateActions();
}

void MacroDialog::UseDefaultSavePath() {
    save_path.clear();
    UpdateSavePathLabel();
    UpdateActions();
}

void MacroDialog::RecordNew() {
    QJsonObject params;
    params.insert(QStringLiteral("name"), name_edit->text().trimmed());
    params.insert(QStringLiteral("player"), player_combo->currentData().toInt());
    params.insert(QStringLiteral("countdown_ms"), 3000);
    if (!save_path.isEmpty()) {
        params.insert(QStringLiteral("path"), save_path);
    }
    QJsonObject result;
    if (!Call(QStringLiteral("macro.record"), params, result)) {
        return;
    }
    if (!result.contains(QStringLiteral("state"))) {
        result = recorder.Status();
    }
    ApplyStatus(result);
    if (result.value(QStringLiteral("state")).toString() == QStringLiteral("error")) {
        return;
    }
    save_path.clear();
    UpdateSavePathLabel();
    YieldToGame();
}

void MacroDialog::PlaySelected() {
    const QString path = SelectedPath();
    if (path.isEmpty()) {
        ShowError(tr("Choose a macro to play."));
        return;
    }
    QJsonObject params;
    params.insert(QStringLiteral("path"), path);
    params.insert(QStringLiteral("player"), player_combo->currentData().toInt());
    QJsonObject result;
    if (!Call(QStringLiteral("macro.play"), params, result)) {
        return;
    }
    if (!result.contains(QStringLiteral("state"))) {
        result = recorder.Status();
    }
    ApplyStatus(result);
    if (result.value(QStringLiteral("state")).toString() == QStringLiteral("error")) {
        return;
    }
    YieldToGame();
}

void MacroDialog::RecordFromMarker() {
    const QString path = SelectedPath();
    if (path.isEmpty()) {
        ShowError(tr("Choose the macro to record from."));
        return;
    }
    const QString marker = marker_combo->currentData().toString();
    if (marker.isEmpty()) {
        ShowError(tr("Choose a marker on that macro."));
        return;
    }
    QJsonObject params;
    params.insert(QStringLiteral("path"), path);
    params.insert(QStringLiteral("marker"), marker);
    params.insert(QStringLiteral("new_name"), name_edit->text().trimmed());
    params.insert(QStringLiteral("player"), player_combo->currentData().toInt());
    params.insert(QStringLiteral("countdown_ms"), 3000);
    if (!save_path.isEmpty()) {
        params.insert(QStringLiteral("output_path"), save_path);
    }
    QJsonObject result;
    if (!Call(QStringLiteral("macro.record_from"), params, result)) {
        return;
    }
    if (!result.contains(QStringLiteral("state"))) {
        result = recorder.Status();
    }
    ApplyStatus(result);
    if (result.value(QStringLiteral("state")).toString() == QStringLiteral("error")) {
        return;
    }
    save_path.clear();
    UpdateSavePathLabel();
    YieldToGame();
}

void MacroDialog::Stop() {
    QJsonObject result;
    if (!Call(QStringLiteral("macro.stop"), QJsonObject{}, result)) {
        return;
    }
    if (!result.contains(QStringLiteral("state"))) {
        result = recorder.Status();
    }
    ApplyStatus(result);
}

void MacroDialog::AddMarker() {
    QJsonObject params;
    const QString label = marker_name_edit->text().trimmed();
    if (!label.isEmpty()) {
        params.insert(QStringLiteral("name"), label);
    }
    QJsonObject result;
    if (!Call(QStringLiteral("macro.marker"), params, result)) {
        return;
    }
    marker_name_edit->clear();
    recent_marker = MarkerNameFrom(result);
    if (!result.contains(QStringLiteral("state"))) {
        result = recorder.Status();
    }
    ApplyStatus(result);
}

void MacroDialog::SaveBinding() {
    const QString path = SelectedPath();
    if (path.isEmpty()) {
        ShowError(tr("Choose a macro first."));
        return;
    }
    QJsonObject params;
    params.insert(QStringLiteral("path"), path);
    params.insert(QStringLiteral("keyboard"),
                  keyboard_binding->keySequence().toString(QKeySequence::PortableText));
    params.insert(QStringLiteral("gamepad"), gamepad_binding->text().trimmed());
    QJsonObject result;
    if (!Call(QStringLiteral("macro.bind"), params, result)) {
        return;
    }
    ReloadLibrary(path);
}

void MacroDialog::SaveMarkerShortcuts() {
    QJsonObject params;
    params.insert(QStringLiteral("marker_keyboard"),
                  marker_keyboard->keySequence().toString(QKeySequence::PortableText));
    params.insert(QStringLiteral("marker_gamepad"), marker_gamepad->text().trimmed());
    QJsonObject result;
    if (!Call(QStringLiteral("macro.shortcuts"), params, result)) {
        return;
    }
    if (result.contains(QStringLiteral("marker_keyboard")) ||
        result.contains(QStringLiteral("marker_gamepad")) ||
        result.contains(QStringLiteral("stop_keyboard"))) {
        ApplyShortcutResult(result);
        return;
    }
    ReloadShortcuts();
}

bool MacroDialog::Call(const QString& method, const QJsonObject& params, QJsonObject& result) {
    ++inside_call;
    const QJsonObject envelope = recorder.Handle(method, params);
    --inside_call;
    if (!envelope.value(QStringLiteral("ok")).toBool()) {
        const QJsonObject error = envelope.value(QStringLiteral("error")).toObject();
        QString message = error.value(QStringLiteral("message")).toString();
        if (message.isEmpty()) {
            message = tr("That action could not be completed.");
        }
        ShowError(message);
        return false;
    }
    const QJsonValue value = envelope.value(QStringLiteral("result"));
    result = value.isObject() ? value.toObject() : QJsonObject{};
    return true;
}

void MacroDialog::ShowError(const QString& message) {
    if (message.isEmpty()) {
        return;
    }
    if (!isVisible()) {
        pending_error = message;
        return;
    }
    QMessageBox::warning(this, tr("Input Macros"), message);
}

void MacroDialog::ShowListErrors(const QJsonArray& errors) {
    if (errors.isEmpty()) {
        return;
    }
    QStringList lines;
    for (qsizetype i = 0; i < errors.size(); ++i) {
        const QJsonValue value = errors.at(i);
        if (value.isString()) {
            const QString line = value.toString();
            if (!line.isEmpty()) {
                lines.append(line);
            }
        } else if (value.isObject()) {
            const QString line = value.toObject().value(QStringLiteral("message")).toString();
            if (!line.isEmpty()) {
                lines.append(line);
            }
        }
    }
    if (lines.isEmpty()) {
        return;
    }
    ShowError(lines.join(QStringLiteral("\n")));
}

QString MacroDialog::SelectedPath() const {
    const int row = library->currentRow();
    if (row < 0) {
        return {};
    }
    const QTableWidgetItem* item = library->item(row, 0);
    if (item == nullptr) {
        return {};
    }
    return item->data(Qt::UserRole).toString();
}

QJsonObject MacroDialog::SelectedEntry() const {
    return library_rows.value(SelectedPath());
}
