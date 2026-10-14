// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class MacroRecorder;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;

// Modeless library for input macros. The parent shows it; it never calls exec().
// The parent owns this object and the MacroRecorder it references.
class MacroDialog final : public QDialog {
    Q_OBJECT

public:
    explicit MacroDialog(MacroRecorder& recorder, QWidget* parent = nullptr,
                         QWidget* game_window = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void BuildUi();
    void OnRecorderChanged();
    void ApplyStatus(const QJsonObject& status);
    void UpdateActions();
    void ReloadLibrary(const QString& preferred_path = {});
    void ReloadShortcuts();
    void ApplyShortcutResult(const QJsonObject& result);
    void LoadSelectionIntoEditors();
    void UpdateSavePathLabel();
    void YieldToGame();

    void Refresh();
    void OpenFile();
    void OpenFolder();
    void ChooseSavePath();
    void UseDefaultSavePath();
    void RecordNew();
    void PlaySelected();
    void RecordFromMarker();
    void Stop();
    void AddMarker();
    void SaveBinding();
    void SaveMarkerShortcuts();

    bool Call(const QString& method, const QJsonObject& params, QJsonObject& result);
    void ShowError(const QString& message);
    void ShowListErrors(const QJsonArray& errors);
    QString Describe(const QJsonObject& status) const;
    QString SelectedPath() const;
    QJsonObject SelectedEntry() const;

    MacroRecorder& recorder;
    QWidget* game_window{};
    QLabel* help_label{};
    QLabel* status_label{};
    QLabel* save_path_label{};
    QLabel* gamepad_hint{};
    QLabel* stop_hint{};
    QTableWidget* library{};
    QComboBox* marker_combo{};
    QComboBox* player_combo{};
    QLineEdit* name_edit{};
    QLineEdit* marker_name_edit{};
    QLineEdit* gamepad_binding{};
    QLineEdit* marker_gamepad{};
    QKeySequenceEdit* keyboard_binding{};
    QKeySequenceEdit* marker_keyboard{};
    QPushButton* refresh_button{};
    QPushButton* open_file_button{};
    QPushButton* open_folder_button{};
    QPushButton* choose_save_button{};
    QPushButton* default_save_button{};
    QPushButton* record_button{};
    QPushButton* play_button{};
    QPushButton* branch_button{};
    QPushButton* stop_button{};
    QPushButton* add_marker_button{};
    QPushButton* save_binding_button{};
    QPushButton* save_marker_shortcuts_button{};

    QHash<QString, QJsonObject> library_rows;
    QStringList session_paths;
    QString macros_directory;
    QString save_path;
    QString applied_state;
    QString pending_error;
    QString recent_marker;
    bool loading{false};
    bool error_posted{false};
    int inside_call{0};
};
