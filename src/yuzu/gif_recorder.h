// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QThread>
#include <QTimer>

class GRenderWindow;

// All public methods run on the GUI thread. Encoding and file I/O run on worker_thread.
class GifRecorder final : public QObject {
    Q_OBJECT
public:
    explicit GifRecorder(GRenderWindow& window, QObject* parent = nullptr);
    ~GifRecorder() override;
    bool Start(const QString& path, int fps, int width, int duration_ms, QString& error);
    void Stop();
    bool IsRecording() const;
    QJsonObject Status() const;

signals:
    void Changed();

private:
    void Capture();
    struct Encoder;
    GRenderWindow& window;
    std::unique_ptr<Encoder> encoder;
    QThread worker_thread;
    QObject* worker{};
    QTimer timer;
    QElapsedTimer clock;
    QString state{QStringLiteral("idle")};
    QString path;
    QString failure;
    int width{}, height{}, fps{}, duration_ms{}, frames{}, dropped{};
    qint64 elapsed_ms{};
    quint64 generation{};
    bool busy{};
};
