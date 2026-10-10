// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QMap>
#include <QPointer>
#include <QTimer>

class QLocalSocket;

// Opt-in, same-user local endpoint. All UI work runs on the GUI thread.
class ControlServer final : public QObject {
public:
    explicit ControlServer(QObject* parent = nullptr);
    ~ControlServer() override;
    bool Listen(const QString& socket, const QString& log_path, QString* error);
    void SetHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler);
    QJsonObject Enqueue(const QString& method, std::function<QJsonObject()> operation);
    static QJsonObject Success(const QJsonValue& result = QJsonObject{});
    static QJsonObject Error(const QString& code, const QString& message);
    static bool BackgroundMode();

private:
    bool eventFilter(QObject* object, QEvent* event) override;
    QString Id(QObject* object);
    QList<QObject*> Objects();
    QObject* Resolve(const QString& target);
    QJsonObject Describe(QObject* object, bool brief);
    QJsonObject Dispatch(const QString& method, const QJsonObject& params);
    QJsonObject Screenshot(const QJsonObject& params);
    QJsonObject Trigger(QObject* target);
    void Accept();
    void Drop(QLocalSocket* socket);
    void Read(QLocalSocket* socket);
    void Reply(QLocalSocket* socket, const QJsonObject& response);
    void Log(const QJsonObject& event);

    QLocalServer listener;
    QFile log;
    QString log_path;
    QElapsedTimer elapsed;
    QHash<QObject*, QString> ids;
    QHash<QString, QPointer<QObject>> objects;
    QHash<QLocalSocket*, QByteArray> buffers;
    QMap<quint64, QJsonObject> jobs;
    std::function<QJsonObject(const QString&, const QJsonObject&)> handler;
    quint64 next_object{1};
    quint64 next_job{1};
    int active_jobs{};
    int idle_timeout_ms{15000};
};
