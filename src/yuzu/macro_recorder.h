// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include "yuzu/macro_file.h"

class GRenderWindow;
class QLabel;
class QShortcut;
class QWidget;
namespace InputCommon { class InputSubsystem; }

// Commands and Tick run on the GUI thread. AdvanceFrame runs on the renderer thread,
// under GRenderWindow's callback lock, and only touches the mutex-protected timeline.
// Destroy this before GRenderWindow and before the owner widget. The destructor clears
// the frame callback before taking mutex or deleting Qt objects.
class MacroRecorder final : public QObject {
    Q_OBJECT
public:
    MacroRecorder(GRenderWindow& render, InputCommon::InputSubsystem& input, QWidget& owner);
    ~MacroRecorder() override;
    QJsonObject Handle(const QString& method, const QJsonObject& params = {});
    QJsonObject Status() const;
    bool IsActive() const;
    bool OwnsInput() const;
    void Stop();
    void AdvanceFrame();

signals:
    void Changed();
    void PlaybackStarting(int player);

private:
    struct Binding {
        QString path;
        int player{};
        quint32 buttons{};
        bool held{};
        QPointer<QShortcut> keyboard;
    };
    bool Ready(QString& error) const;
    QString Resolve(const QJsonObject& params, QString& error) const;
    QString OutputPath(const QString& name, const QString& requested, QString& error) const;
    bool ReadDocument(const QJsonObject& params, EdenMacro::Document& doc,
                      QString& path, QString& error) const;
    QJsonObject List() const;
    QStringList LibraryFiles() const;
    bool RememberPath(const QString& file, QString& error);
    bool StartRecord(const QJsonObject& params, bool branch, QString& error);
    bool Play(const QJsonObject& params, QString& error);
    bool AddMarker(const QString& name, QString& error);
    bool Bind(const QJsonObject& params, QString& error);
    bool SetShortcuts(const QJsonObject& params, QString& error);
    void ReloadBindings();
    void Tick();
    void Finish();
    void Cancel(); // mutex not held
    void BeginAfterCountdown();
    void Neutral(); // called with mutex held
    void ReleaseExclusive(); // called with mutex held
    void RecordOne(); // called with mutex held
    void PlayOne(); // called with mutex held
    EdenMacro::Sample Capture(int player) const;
    bool Apply(const EdenMacro::Sample& sample); // called with mutex held
    bool ParseGamepad(const QString& text, quint32& buttons, QString& error) const;
    void ShowIndicator(const QJsonObject& status);
    void PollChords();
    void ApplyShortcutGates();
    void PublishChange();
    void NoteUi();
    void LoadShortcutFile();
    void WriteShortcutFile(QString& error) const;
    quint32 ButtonMask(int index) const;
    qint64 CountdownRemaining() const; // mutex held
    QString Cue() const; // mutex held

    GRenderWindow& render;
    InputCommon::InputSubsystem& input;
    QWidget& owner;
    QString directory;
    mutable std::mutex mutex;
    EdenMacro::Document document;
    EdenMacro::Document source;
    QString state{QStringLiteral("idle")};
    QString path;
    QString failure;
    QString branch_marker;
    quint64 frame{};
    quint64 prefix_frames{};
    int player{};
    bool branch_pending{};
    bool save_pending{};
    bool injected{};
    std::atomic_bool can_advance{false};
    QElapsedTimer countdown;
    int countdown_ms{};
    qint64 countdown_spent{0};
    bool rearm_pending{false};
    bool exclusive{false};
    bool shutting_down{false};
    QTimer timer;
    QPointer<QLabel> indicator;
    QPointer<QShortcut> marker_shortcut;
    QPointer<QShortcut> stop_shortcut;
    QString marker_keyboard{QStringLiteral("Ctrl+Shift+M")};
    QString marker_gamepad;
    quint32 marker_buttons{};
    bool marker_held{};
    bool shortcuts_armed{true};
    std::vector<Binding> bindings;
    QString directory_error;
    QStringList linked_files;
    bool refresh_user_pending{false};
    // GUI thread only. AdvanceFrame does not touch these.
    mutable QString command_code;
    bool entering{false};
    QString ui_state;
    QString ui_cue;
    quint64 ui_frame{};
    int ui_countdown{-1};
};
