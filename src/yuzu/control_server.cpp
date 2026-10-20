// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLibrary>
#include <QLineEdit>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMetaProperty>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QScreen>
#include <QtGui/qguiapplication_platform.h>
#include <QSet>
#include <QShortcut>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextEdit>
#include <QTreeView>
#include <QWindow>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#include "yuzu/control_server.h"

namespace {
constexpr qsizetype MaxRequest = 1024 * 1024;
constexpr qsizetype MaxResponse = 16 * 1024 * 1024;
constexpr qint64 MaxLog = 4 * 1024 * 1024;
constexpr int MaxConnections = 32;
constexpr int MaxActiveJobs = 32;
constexpr int MaxJobHistory = 128;
constexpr int RequestsPerRead = 16;
constexpr int DefaultIdleTimeoutMs = 15000;

bool Integer(const QJsonValue& value, int minimum, int maximum) {
    return value.isDouble() && std::isfinite(value.toDouble()) &&
           std::trunc(value.toDouble()) == value.toDouble() &&
           value.toDouble() >= minimum && value.toDouble() <= maximum;
}

bool Scalar(const QMetaProperty& property) {
    const auto type = property.metaType();
    return property.isEnumType() || type == QMetaType::fromType<bool>() ||
           type == QMetaType::fromType<int>() || type == QMetaType::fromType<uint>() ||
           type == QMetaType::fromType<double>() || type == QMetaType::fromType<float>() ||
           type == QMetaType::fromType<QString>();
}

QWidget* WidgetFor(QObject* object) {
    for (QObject* current = object; current; current = current->parent()) {
        if (auto* widget = qobject_cast<QWidget*>(current)) {
            return widget;
        }
    }
    return nullptr;
}

// True when `window` would be a child window (transient) of `ancestor`, e.g. a dialog opened
// from a modal dialog. Qt never blocks a modal window's own descendants.
bool DescendsFrom(QWidget* window, QWidget* ancestor) {
    for (QWidget* current = window; current; current = current->parentWidget() ? current->parentWidget()->window() : nullptr) {
        if (current == ancestor) {
            return true;
        }
    }
    return false;
}

// Mirrors Qt's modality rules: an application-modal window blocks every other top level;
// a window-modal window blocks only the windows in its parent chain.
bool BlockedByModal(QWidget* window, QWidget* modal) {
    if (!window || !modal) {
        return false;
    }
    QWidget* modal_window = modal->window();
    if (DescendsFrom(window, modal_window)) {
        return false;
    }
    switch (modal->windowModality()) {
    case Qt::NonModal:
        return false;
    case Qt::WindowModal:
        for (QWidget* parent = modal_window->parentWidget(); parent; parent = parent->window()->parentWidget()) {
            if (parent->window() == window) {
                return true;
            }
        }
        return false;
    default:
        return true;
    }
}

bool WindowReachable(QWidget* window) {
    if (!window) {
        return true;
    }
    if (auto* popup = QApplication::activePopupWidget(); popup && popup->window() == window) {
        return true; // an open context or menubar menu is always on top
    }
    return !BlockedByModal(window, QApplication::activeModalWidget());
}

bool Reachable(QObject* object) {
    if (auto* action = qobject_cast<QAction*>(object)) {
        bool any_widget = false;
        for (auto* associated : action->associatedObjects()) {
            if (auto* widget = WidgetFor(associated)) {
                any_widget = true;
                if (WindowReachable(widget->window())) {
                    return true;
                }
            }
        }
        if (any_widget) {
            return false;
        }
    }
    auto* widget = WidgetFor(object);
    return WindowReachable(widget ? widget->window() : nullptr);
}

// The visible popup menu that currently shows this action, if any.
QMenu* VisiblePopupMenuFor(QAction* action) {
    QMenu* fallback = nullptr;
    auto* popup = QApplication::activePopupWidget();
    for (auto* associated : action->associatedObjects()) {
        auto* menu = qobject_cast<QMenu*>(associated);
        if (!menu || !menu->isVisible()) {
            continue;
        }
        if (popup && menu->window() == popup->window()) {
            return menu;
        }
        fallback = fallback ? fallback : menu;
    }
    return fallback;
}

QWidget* EventReceiver(QWidget* widget) {
    if (auto* area = qobject_cast<QAbstractScrollArea*>(widget)) {
        return area->viewport();
    }
    return widget;
}

QModelIndex Index(QAbstractItemView* view, const QJsonObject& params) {
    if (!view->model()) {
        return {};
    }
    QModelIndex parent = view->rootIndex();
    const auto path = params.value(QStringLiteral("parents"));
    if (!path.isUndefined() && !path.isArray()) {
        return {};
    }
    if (path.toArray().size() > 64) {
        return {};
    }
    for (const auto row : path.toArray()) {
        if (!Integer(row, 0, view->model()->rowCount(parent) - 1)) {
            return {};
        }
        parent = view->model()->index(row.toInt(), 0, parent);
        if (!parent.isValid()) {
            return {};
        }
    }
    const auto row = params.value(QStringLiteral("row"));
    const auto column = params.value(QStringLiteral("column")).isUndefined() ? QJsonValue{0} : params.value(QStringLiteral("column"));
    if (!Integer(row, 0, view->model()->rowCount(parent) - 1) ||
        !Integer(column, 0, view->model()->columnCount(parent) - 1)) {
        return {};
    }
    return view->model()->index(row.toInt(), column.toInt(), parent);
}

// QLocalServer with access options binds a temporary name and renames it over the requested
// path, so listen() would silently take over a live endpoint. Probe first and reclaim only a
// socket file nobody answers on.
bool ReclaimSocketPath(const QString& path, QString* error) {
#ifdef Q_OS_UNIX
    struct stat info {};
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0) {
        return true; // absent: listen() reports any other problem itself
    }
    if (!S_ISSOCK(info.st_mode)) {
        *error = QStringLiteral("socket_path_occupied: %1 exists and is not a socket").arg(path);
        return false;
    }
    QLocalSocket probe;
    probe.connectToServer(path);
    if (probe.waitForConnected(500)) {
        probe.disconnectFromServer();
        *error = QStringLiteral("socket_in_use: another process is serving %1; stop it or choose another path").arg(path);
        return false;
    }
    if (probe.error() != QLocalSocket::ConnectionRefusedError && probe.error() != QLocalSocket::ServerNotFoundError) {
        *error = QStringLiteral("socket_in_use: %1 did not refuse the connection (%2); not reclaiming").arg(path, probe.errorString());
        return false;
    }
    if (!QFile::remove(path)) {
        *error = QStringLiteral("socket_stale: could not remove dead socket %1").arg(path);
        return false;
    }
#else
    Q_UNUSED(path);
    Q_UNUSED(error);
#endif
    return true;
}

// True when every pixel has the same colour (alpha ignored): the signature of a failed grab.
bool Uniform(const QImage& image) {
    if (image.isNull()) {
        return true;
    }
    const QImage rgb = image.format() == QImage::Format_RGB32 ? image : image.convertToFormat(QImage::Format_RGB32);
    const QRgb first = rgb.pixel(0, 0) | 0xff000000u;
    for (int y = 0; y < rgb.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); ++x) {
            if ((line[x] | 0xff000000u) != first) {
                return false;
            }
        }
    }
    return true;
}

#if QT_CONFIG(xcb)
// QScreen::grabWindow on xcb copies from the root window whenever the target shares the root's
// depth (to include overlapping windows). Under Xwayland the root has no contents, so every
// grab of an Eden window came back black. Reading the window's own drawable returns its
// composited pixels, including native Vulkan/OpenGL children and the last presented frame of a
// paused game. libxcb is resolved at runtime so no build dependency is added.
struct Xcb {
    struct Cookie {
        unsigned int sequence;
    };
    struct ImageReply {
        uint8_t response_type;
        uint8_t depth;
        uint16_t sequence;
        uint32_t length;
        uint32_t visual;
        uint8_t pad0[20];
    };
    struct GeometryReply {
        uint8_t response_type;
        uint8_t depth;
        uint16_t sequence;
        uint32_t length;
        uint32_t root;
        int16_t x;
        int16_t y;
        uint16_t width;
        uint16_t height;
        uint16_t border_width;
        uint8_t pad0[2];
    };
    Cookie (*get_geometry)(void*, uint32_t){};
    GeometryReply* (*get_geometry_reply)(void*, Cookie, void**){};
    Cookie (*get_image)(void*, uint8_t, uint32_t, int16_t, int16_t, uint16_t, uint16_t, uint32_t){};
    ImageReply* (*get_image_reply)(void*, Cookie, void**){};
    uint8_t* (*get_image_data)(const ImageReply*){};
    int (*get_image_data_length)(const ImageReply*){};
    bool loaded{};

    static const Xcb& Instance() {
        static const Xcb instance = [] {
            Xcb result;
            QLibrary library(QStringLiteral("xcb"), 1);
            if (!library.load()) {
                return result;
            }
            result.get_geometry = reinterpret_cast<decltype(result.get_geometry)>(library.resolve("xcb_get_geometry"));
            result.get_geometry_reply = reinterpret_cast<decltype(result.get_geometry_reply)>(library.resolve("xcb_get_geometry_reply"));
            result.get_image = reinterpret_cast<decltype(result.get_image)>(library.resolve("xcb_get_image"));
            result.get_image_reply = reinterpret_cast<decltype(result.get_image_reply)>(library.resolve("xcb_get_image_reply"));
            result.get_image_data = reinterpret_cast<decltype(result.get_image_data)>(library.resolve("xcb_get_image_data"));
            result.get_image_data_length = reinterpret_cast<decltype(result.get_image_data_length)>(library.resolve("xcb_get_image_data_length"));
            result.loaded = result.get_geometry && result.get_geometry_reply && result.get_image &&
                            result.get_image_reply && result.get_image_data && result.get_image_data_length;
            return result;
        }();
        return instance;
    }
};

QImage GrabX11Window(QWidget* window, QString* detail) {
    const auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    const auto& xcb = Xcb::Instance();
    if (!x11 || !x11->connection() || !xcb.loaded) {
        *detail = QStringLiteral("X11 native interface or libxcb unavailable");
        return {};
    }
    void* connection = x11->connection();
    const auto id = static_cast<uint32_t>(window->windowHandle()->winId());
    void* error = nullptr;
    auto* geometry = xcb.get_geometry_reply(connection, xcb.get_geometry(connection, id), &error);
    if (!geometry) {
        const auto error_code = error ? static_cast<const uint8_t*>(error)[1] : 0;
        std::free(error);
        *detail = QStringLiteral("window geometry unavailable (native %1, widget %2, X11 error %3)")
                      .arg(id).arg(window->internalWinId()).arg(error_code);
        return {};
    }
    const uint16_t width = geometry->width;
    const uint16_t height = geometry->height;
    std::free(geometry);
    if (width == 0 || height == 0) {
        *detail = QStringLiteral("window has no size");
        return {};
    }
    constexpr uint8_t z_pixmap = 2;
    auto* reply = xcb.get_image_reply(connection, xcb.get_image(connection, z_pixmap, id, 0, 0, width, height, 0xffffffffu), &error);
    if (!reply) {
        *detail = QStringLiteral("X11 image request failed (native id %1, error %2; window may be hidden or outside the screen)")
                      .arg(id).arg(error ? static_cast<const uint8_t*>(error)[1] : 0);
        std::free(error);
        return {};
    }
    QImage image;
    const int length = xcb.get_image_data_length(reply);
    if ((reply->depth == 24 || reply->depth == 32) && length >= 4 * width * height) {
        image = QImage(xcb.get_image_data(reply), width, height, length / height, QImage::Format_ARGB32).convertToFormat(QImage::Format_RGB32);
        image.setDevicePixelRatio(window->devicePixelRatio());
    } else {
        *detail = QStringLiteral("unsupported window depth %1").arg(reply->depth);
    }
    std::free(reply);
    return image;
}
#endif

// Pixels of one top-level window: the platform's window contents first, then Qt's software
// render of the widget tree when the former is unavailable or uniformly one colour.
QImage CaptureWindow(QWidget* window, QString* backend, QString* warning) {
    QImage image;
    QString detail;
    if (window->isVisible() && window->windowHandle()) {
#if QT_CONFIG(xcb)
        if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
            image = GrabX11Window(window, &detail);
            *backend = QStringLiteral("x11-window");
        } else
#endif
        if (auto* screen = window->screen()) {
            image = screen->grabWindow(window->winId()).toImage();
            *backend = QStringLiteral("screen-window");
            if (image.isNull()) {
                detail = QStringLiteral("platform grab returned nothing");
            }
        }
    } else {
        detail = QStringLiteral("window is hidden");
    }
    if (image.isNull() || Uniform(image)) {
        const QImage rendered = window->grab().toImage();
        if (image.isNull() || (!rendered.isNull() && !Uniform(rendered))) {
            *warning = (image.isNull() ? QStringLiteral("window capture unavailable (%1)").arg(detail)
                                       : QStringLiteral("window capture was a single colour"))
                       + QStringLiteral("; showing Qt-rendered widgets, native GPU surfaces may be absent");
            image = rendered;
            *backend = QStringLiteral("widget-render");
        }
    }
    return image;
}

QImage CropToWidget(const QImage& image, QWidget* widget, QWidget* window) {
    if (image.isNull() || widget == window) {
        return image;
    }
    const qreal ratio = image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
    const QRect logical{widget->mapTo(window, QPoint{}), widget->size()};
    const QRect device{QPoint{qRound(logical.x() * ratio), qRound(logical.y() * ratio)},
                       QSize{qRound(logical.width() * ratio), qRound(logical.height() * ratio)}};
    QImage cropped = image.copy(device & image.rect());
    cropped.setDevicePixelRatio(ratio);
    return cropped;
}

// Painting order for the composite of every visible top level: main windows under dialogs,
// modal dialogs under popups.
int StackRank(QWidget* window) {
    if (window->windowType() == Qt::Popup) {
        return 3;
    }
    if (window->windowModality() != Qt::NonModal) {
        return 2;
    }
    if (qobject_cast<QMainWindow*>(window)) {
        return 0;
    }
    return 1;
}
} // namespace

ControlServer::ControlServer(QObject* parent) : QObject{parent} {
    if (BackgroundMode()) {
        qApp->installEventFilter(this);
    }
    elapsed.start();
    bool ok = false;
    const int configured = qEnvironmentVariableIntValue("EDEN_CONTROL_IDLE_TIMEOUT_MS", &ok);
    idle_timeout_ms = ok ? qBound(1000, configured, 600000) : DefaultIdleTimeoutMs;
    connect(&listener, &QLocalServer::newConnection, this, [this] { Accept(); });
}

bool ControlServer::BackgroundMode() {
    return qApp && qApp->property("edenControlBackground").toBool();
}

bool ControlServer::eventFilter(QObject* object, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(object);
    if (widget && widget->isWindow()) {
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
            widget->setAttribute(Qt::WA_ShowWithoutActivating);
            widget->setAttribute(Qt::WA_X11DoNotAcceptFocus);
            // Polish precedes the first show. Let QWidget recreate its native
            // handle if necessary; changing QWindow flags behind its back can
            // leave QWidget::winId() pointing at a destroyed X11 drawable.
            if (event->type() == QEvent::Polish) {
                widget->setWindowFlags((widget->windowFlags() & ~Qt::WindowStaysOnTopHint) |
                                       Qt::WindowDoesNotAcceptFocus | Qt::WindowStaysOnBottomHint);
            }
        }
        if (event->type() == QEvent::Show) {
            // Qt raises popup windows internally, independently of Eden's
            // explicit raise calls. Lower after Qt finishes mapping the popup.
            QPointer<QWidget> window{widget};
            QTimer::singleShot(0, this, [window] {
                if (window && window->isVisible()) {
                    window->lower();
                }
            });
        }
    }
    return QObject::eventFilter(object, event);
}

ControlServer::~ControlServer() {
    qApp->removeEventFilter(this);
    listener.close();
    // Accepted sockets belong to listener, which outlives buffers during member
    // destruction. Their destructors emit disconnected(); detach and destroy them
    // while our state is still alive, before Drop could access a destroyed hash.
    for (auto* socket : listener.findChildren<QLocalSocket*>()) {
        socket->disconnect(this);
        delete socket;
    }
    buffers.clear();
    Log({{QStringLiteral("event"), QStringLiteral("shutdown")}});
}

void ControlServer::Accept() {
    while (listener.hasPendingConnections()) {
        auto* socket = listener.nextPendingConnection();
        if (buffers.size() >= MaxConnections) {
            Log({{QStringLiteral("event"), QStringLiteral("connection_refused")}, {QStringLiteral("open"), static_cast<int>(buffers.size())}});
            socket->abort();
            socket->deleteLater();
            continue;
        }
        socket->setReadBufferSize(MaxRequest + 1);
        buffers.insert(socket, {});
        auto* deadline = new QTimer(socket);
        deadline->setSingleShot(true);
        deadline->setInterval(idle_timeout_ms);
        connect(deadline, &QTimer::timeout, this, [this, socket] {
            Log({{QStringLiteral("event"), QStringLiteral("connection_idle")}});
            Drop(socket);
        });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, deadline] {
            deadline->start();
            Read(socket);
        });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] { Drop(socket); });
        deadline->start();
        if (socket->bytesAvailable()) {
            Read(socket);
        }
    }
}

void ControlServer::Drop(QLocalSocket* socket) {
    if (buffers.remove(socket) == 0) {
        return; // already dropped; abort() may re-enter through disconnected()
    }
    socket->abort();
    socket->deleteLater();
}

bool ControlServer::Listen(const QString& socket, const QString& path, QString* error) {
    if (socket.isEmpty() || !QDir::isAbsolutePath(socket)) {
        *error = QStringLiteral("--control-socket requires an absolute local socket path");
        return false;
    }
    if (!path.isEmpty()) {
        log_path = path;
        log.setFileName(log_path);
        if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) {
            *error = log.errorString();
            return false;
        }
        log.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    if (!ReclaimSocketPath(socket, error)) {
        Log({{QStringLiteral("event"), QStringLiteral("listen_refused")}, {QStringLiteral("error"), *error}});
        return false;
    }
    listener.setSocketOptions(QLocalServer::UserAccessOption);
    listener.setMaxPendingConnections(16);
    if (!listener.listen(socket)) {
        *error = listener.errorString() + QStringLiteral("; another instance or a stale socket may own this path. Use a fresh path.");
        return false;
    }
    Log({{QStringLiteral("event"), QStringLiteral("listen")},
         {QStringLiteral("socket"), listener.fullServerName()},
         {QStringLiteral("pid"), QCoreApplication::applicationPid()},
         {QStringLiteral("idle_timeout_ms"), idle_timeout_ms}});
    return true;
}

void ControlServer::SetHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> value) {
    handler = std::move(value);
}

QJsonObject ControlServer::Success(const QJsonValue& result) {
    return {{QStringLiteral("ok"), true}, {QStringLiteral("result"), result}};
}

QJsonObject ControlServer::Error(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}}};
}

void ControlServer::Log(const QJsonObject& event) {
    if (!log.isOpen()) {
        return;
    }
    if (log.size() >= MaxLog) {
        log.close();
        QFile::remove(log_path + QStringLiteral(".3"));
        for (int i = 2; i >= 1; --i) {
            QFile::rename(log_path + QLatin1Char('.') + QString::number(i), log_path + QLatin1Char('.') + QString::number(i + 1));
        }
        QFile::rename(log_path, log_path + QStringLiteral(".1"));
        if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) {
            qWarning("Control log rotation failed: %s", qPrintable(log.errorString()));
            return;
        }
        log.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    auto entry = event;
    entry.insert(QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    entry.insert(QStringLiteral("elapsed_ms"), elapsed.elapsed());
    log.write(QJsonDocument{entry}.toJson(QJsonDocument::Compact) + '\n');
    log.flush();
}

void ControlServer::Reply(QLocalSocket* socket, const QJsonObject& response) {
    auto bytes = QJsonDocument{response}.toJson(QJsonDocument::Compact);
    if (bytes.size() > MaxResponse) {
        auto error = Error(QStringLiteral("response_too_large"), QStringLiteral("Use a narrower target, brief mode, or a smaller range"));
        error.insert(QStringLiteral("id"), response.value(QStringLiteral("id")));
        bytes = QJsonDocument{error}.toJson(QJsonDocument::Compact);
    }
    if (socket->bytesToWrite() + bytes.size() > MaxResponse) {
        Log({{QStringLiteral("event"), QStringLiteral("connection_backpressure")}});
        Drop(socket);
        return;
    }
    socket->write(bytes + '\n');
    socket->flush();
}

void ControlServer::Read(QLocalSocket* socket) {
    if (!buffers.contains(socket)) {
        return;
    }
    buffers[socket].append(socket->read(MaxRequest + 1 - buffers[socket].size()));
    for (int count = 0; count < RequestsPerRead && buffers.contains(socket); ++count) {
        auto& buffer = buffers[socket];
        const auto newline = buffer.indexOf('\n');
        if (newline < 0) {
            if (buffer.size() > MaxRequest) {
                Reply(socket, Error(QStringLiteral("request_too_large"), QStringLiteral("Request exceeds 1 MiB")));
                if (buffers.contains(socket)) {
                    socket->disconnectFromServer();
                }
            }
            return;
        }
        const auto line = buffer.left(newline);
        buffer.remove(0, newline + 1);
        QJsonParseError parse_error;
        const auto document = QJsonDocument::fromJson(line, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
            Reply(socket, Error(QStringLiteral("invalid_json"), QStringLiteral("Expected one JSON object per line")));
            continue;
        }
        const auto request = document.object();
        const auto method = request.value(QStringLiteral("method"));
        const auto params = request.value(QStringLiteral("params"));
        const auto id = request.value(QStringLiteral("id"));
        if (!method.isString() || method.toString().isEmpty() || method.toString().size() > 128 ||
            (!params.isUndefined() && !params.isObject()) || id.isArray() || id.isObject()) {
            auto response = Error(QStringLiteral("invalid_request"), QStringLiteral("method must be a string, params an object, id a scalar"));
            response.insert(QStringLiteral("id"), id);
            Reply(socket, response);
            continue;
        }
        Log({{QStringLiteral("event"), QStringLiteral("request")}, {QStringLiteral("id"), id},
             {QStringLiteral("method"), method}, {QStringLiteral("target"), params.toObject().value(QStringLiteral("target"))}});
        QJsonObject response;
        try {
            response = Dispatch(method.toString(), params.toObject());
        } catch (const std::exception& exception) {
            response = Error(QStringLiteral("exception"), QString::fromUtf8(exception.what()));
        }
        response.insert(QStringLiteral("id"), id);
        Log({{QStringLiteral("event"), QStringLiteral("reply")}, {QStringLiteral("id"), id},
             {QStringLiteral("ok"), response.value(QStringLiteral("ok"))},
             {QStringLiteral("error"), response.value(QStringLiteral("error"))}});
        if (buffers.contains(socket)) {
            Reply(socket, response);
        }
    }
    if (buffers.contains(socket) && (buffers[socket].contains('\n') || socket->bytesAvailable())) {
        QTimer::singleShot(0, socket, [this, socket] { Read(socket); });
    }
}

QString ControlServer::Id(QObject* object) {
    if (!object) {
        return {};
    }
    if (ids.contains(object)) {
        return ids.value(object);
    }
    const auto id = QStringLiteral("o%1").arg(next_object++);
    ids.insert(object, id);
    objects.insert(id, object);
    connect(object, &QObject::destroyed, this, [this, object, id] {
        ids.remove(object);
        objects.remove(id);
    });
    return id;
}

QList<QObject*> ControlServer::Objects() {
    QList<QObject*> result;
    QSet<QObject*> seen;
    const auto append = [&](QObject* object) {
        if (object && !seen.contains(object)) {
            seen.insert(object);
            result.append(object);
            Id(object);
        }
    };
    for (auto* widget : QApplication::allWidgets()) {
        append(widget);
        for (auto* action : widget->actions()) {
            append(action); // actions shown by a widget but parented elsewhere
        }
    }
    // One recursive walk per top level covers actions and shortcuts owned by any QObject in
    // the tree (action groups, dialogs) without rescanning every subtree.
    for (auto* top : QApplication::topLevelWidgets()) {
        for (auto* action : top->findChildren<QAction*>()) {
            append(action);
        }
        for (auto* shortcut : top->findChildren<QShortcut*>()) {
            append(shortcut);
        }
    }
    for (auto* action : qApp->findChildren<QAction*>()) {
        append(action);
    }
    for (auto* shortcut : qApp->findChildren<QShortcut*>()) {
        append(shortcut);
    }
    return result;
}

QObject* ControlServer::Resolve(const QString& target) {
    if (target == QStringLiteral("active")) {
        if (auto* modal = QApplication::activeModalWidget()) {
            return modal;
        }
        if (auto* popup = QApplication::activePopupWidget()) {
            return popup;
        }
        if (auto* active = QApplication::activeWindow()) {
            return active;
        }
        // Automation rarely owns keyboard focus; fall back to what is on screen.
        QWidget* fallback = nullptr;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (!widget->isVisible() || !widget->isWindow() || widget->windowType() == Qt::ToolTip || widget->windowType() == Qt::SplashScreen) {
                continue;
            }
            if (qobject_cast<QMainWindow*>(widget)) {
                return widget;
            }
            fallback = fallback ? fallback : widget;
        }
        return fallback;
    }
    if (auto existing = objects.value(target)) {
        return existing;
    }
    QObject* found = nullptr;
    for (auto* object : Objects()) {
        if (!target.isEmpty() && object->objectName() == target) {
            if (found) {
                return nullptr; // Names can be ambiguous across dialogs; IDs never are.
            }
            found = object;
        }
    }
    return found;
}

QJsonObject ControlServer::Describe(QObject* object, bool brief) {
    QJsonObject result{{QStringLiteral("id"), Id(object)}, {QStringLiteral("parent"), Id(object->parent())},
                       {QStringLiteral("class"), QString::fromUtf8(object->metaObject()->className())},
                       {QStringLiteral("name"), object->objectName()}, {QStringLiteral("reachable"), Reachable(object)}};
    if (auto* widget = qobject_cast<QWidget*>(object)) {
        result.insert(QStringLiteral("visible"), widget->isVisible());
        result.insert(QStringLiteral("enabled"), widget->isEnabled());
        result.insert(QStringLiteral("window"), Id(widget->window()));
        result.insert(QStringLiteral("title"), widget->windowTitle());
        result.insert(QStringLiteral("tooltip"), widget->toolTip());
        result.insert(QStringLiteral("geometry"), QJsonArray{widget->x(), widget->y(), widget->width(), widget->height()});
        if (widget->isWindow()) {
            result.insert(QStringLiteral("modality"), static_cast<int>(widget->windowModality()));
            result.insert(QStringLiteral("popup"), (widget->windowType() == Qt::Popup));
        }
    }
    if (auto* action = qobject_cast<QAction*>(object)) {
        result.insert(QStringLiteral("text"), action->text());
        result.insert(QStringLiteral("tooltip"), action->toolTip());
        result.insert(QStringLiteral("enabled"), action->isEnabled());
        result.insert(QStringLiteral("visible"), action->isVisible());
        result.insert(QStringLiteral("checkable"), action->isCheckable());
        result.insert(QStringLiteral("checked"), action->isChecked());
        result.insert(QStringLiteral("separator"), action->isSeparator());
        result.insert(QStringLiteral("shortcut"), action->shortcut().toString());
        if (action->menu()) {
            result.insert(QStringLiteral("menu"), Id(action->menu()));
        }
        QJsonArray associated;
        for (auto* owner : action->associatedObjects()) {
            if (qobject_cast<QWidget*>(owner)) {
                associated.append(Id(owner)); // menus, menubars, toolbars, windows showing this action
            }
        }
        result.insert(QStringLiteral("associated"), associated);
    }
    if (auto* button = qobject_cast<QAbstractButton*>(object)) {
        result.insert(QStringLiteral("text"), button->text());
        result.insert(QStringLiteral("checkable"), button->isCheckable());
        result.insert(QStringLiteral("checked"), button->isChecked());
    }
    if (auto* shortcut = qobject_cast<QShortcut*>(object)) {
        auto* owner = WidgetFor(shortcut->parent());
        result.insert(QStringLiteral("shortcut"), shortcut->key().toString());
        result.insert(QStringLiteral("enabled"), shortcut->isEnabled());
        result.insert(QStringLiteral("visible"), owner && owner->isVisible());
        result.insert(QStringLiteral("context"), static_cast<int>(shortcut->context()));
        result.insert(QStringLiteral("autorepeat"), shortcut->autoRepeat());
        result.insert(QStringLiteral("window"), Id(owner ? owner->window() : nullptr));
    }
    if (brief) {
        return result;
    }
    QJsonObject properties;
    const bool password = qobject_cast<QLineEdit*>(object) && qobject_cast<QLineEdit*>(object)->echoMode() != QLineEdit::Normal;
    for (int i = 0; i < object->metaObject()->propertyCount(); ++i) {
        const auto property = object->metaObject()->property(i);
        if (!property.isReadable() || !Scalar(property)) {
            continue;
        }
        const auto name = QString::fromUtf8(property.name());
        if (password && (name == QStringLiteral("text") || name == QStringLiteral("displayText"))) {
            continue;
        }
        QJsonObject item{{QStringLiteral("value"), QJsonValue::fromVariant(property.read(object))},
                         {QStringLiteral("writable"), property.isWritable()},
                         {QStringLiteral("type"), QString::fromUtf8(property.typeName())}};
        if (property.isEnumType()) {
            QJsonObject values;
            const auto enumeration = property.enumerator();
            for (int j = 0; j < enumeration.keyCount(); ++j) {
                values.insert(QString::fromUtf8(enumeration.key(j)), enumeration.value(j));
            }
            item.insert(QStringLiteral("choices"), values);
        }
        properties.insert(name, item);
    }
    result.insert(QStringLiteral("properties"), properties);
    if (auto* combo = qobject_cast<QComboBox*>(object)) {
        QJsonArray items;
        for (int i = 0; i < std::min(combo->count(), 512); ++i) {
            items.append(QJsonObject{{QStringLiteral("index"), i}, {QStringLiteral("text"), combo->itemText(i)}});
        }
        result.insert(QStringLiteral("items"), items);
    }
    return result;
}

QJsonObject ControlServer::Enqueue(const QString& method, std::function<QJsonObject()> operation) {
    if (active_jobs >= MaxActiveJobs) {
        return Error(QStringLiteral("busy"), QStringLiteral("Too many pending or modal operations"));
    }
    while (jobs.size() >= MaxJobHistory) {
        auto oldest = jobs.begin();
        while (oldest != jobs.end() && (oldest->value(QStringLiteral("state")) == QStringLiteral("running") || oldest->value(QStringLiteral("state")) == QStringLiteral("queued"))) {
            ++oldest;
        }
        if (oldest == jobs.end()) {
            return Error(QStringLiteral("busy"), QStringLiteral("Operation history is full"));
        }
        jobs.erase(oldest);
    }
    const auto job = next_job++;
    jobs.insert(job, {{QStringLiteral("job"), QString::number(job)}, {QStringLiteral("method"), method}, {QStringLiteral("state"), QStringLiteral("queued")}});
    ++active_jobs;
    // Jobs deliberately run from the event loop that is current when the timer fires, so a
    // dialog opened by one job can be driven to completion by later jobs.
    QTimer::singleShot(0, this, [this, job, operation = std::move(operation)] {
        jobs[job].insert(QStringLiteral("state"), QStringLiteral("running"));
        Log({{QStringLiteral("event"), QStringLiteral("job_started")}, {QStringLiteral("job"), QString::number(job)}});
        QJsonObject response;
        try {
            response = operation();
        } catch (const std::exception& exception) {
            response = Error(QStringLiteral("exception"), QString::fromUtf8(exception.what()));
        }
        --active_jobs;
        jobs[job].insert(QStringLiteral("state"), response.value(QStringLiteral("ok")).toBool() ? QStringLiteral("done") : QStringLiteral("error"));
        jobs[job].insert(QStringLiteral("response"), response);
        Log({{QStringLiteral("event"), QStringLiteral("job_finished")}, {QStringLiteral("job"), QString::number(job)}, {QStringLiteral("ok"), response.value(QStringLiteral("ok"))}});
    });
    return Success(QJsonObject{{QStringLiteral("accepted"), true}, {QStringLiteral("job"), QString::number(job)}});
}

QJsonObject ControlServer::Screenshot(const QJsonObject& params) {
    const auto path = params.value(QStringLiteral("path")).toString();
    const auto target = params.value(QStringLiteral("target")).toString(QStringLiteral("active"));
    if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
        return Error(QStringLiteral("invalid_path"), QStringLiteral("An absolute PNG output path is required"));
    }
    QImage capture;
    QString backend;
    QString warning;
    QString target_id;
    QJsonArray windows;
    if (target == QStringLiteral("screen")) {
        if (auto* screen = QApplication::primaryScreen()) {
            capture = screen->grabWindow(0).toImage();
            backend = QStringLiteral("screen");
            if (!capture.isNull() && Uniform(capture)) {
                warning = QStringLiteral("root window capture is a single colour; rootless X servers such as Xwayland have no root contents, use all or a window target");
            }
        }
    } else if (target == QStringLiteral("all")) {
        QList<QWidget*> visible;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (widget->isVisible() && widget->isWindow() && widget->windowType() != Qt::ToolTip && widget->windowType() != Qt::SplashScreen) {
                visible.append(widget);
            }
        }
        if (visible.isEmpty()) {
            return Error(QStringLiteral("capture_failed"), QStringLiteral("No visible window"));
        }
        std::stable_sort(visible.begin(), visible.end(), [](QWidget* a, QWidget* b) { return StackRank(a) < StackRank(b); });
        QRect bounds;
        for (auto* widget : visible) {
            bounds |= widget->geometry();
        }
        const qreal ratio = visible.first()->devicePixelRatio();
        const QSize canvas_size{qRound(bounds.width() * ratio), qRound(bounds.height() * ratio)};
        if (canvas_size.width() <= 0 || canvas_size.height() <= 0 || canvas_size.width() > 16384 || canvas_size.height() > 16384) {
            return Error(QStringLiteral("capture_failed"), QStringLiteral("Window layout is too large to composite"));
        }
        capture = QImage(canvas_size, QImage::Format_RGB32);
        // Paint in logical coordinates on a DPR-aware canvas. Setting the ratio only after
        // painting made a 2x source occupy one quarter of the image on HiDPI displays.
        capture.setDevicePixelRatio(ratio);
        capture.fill(Qt::black);
        QPainter painter(&capture);
        for (auto* widget : visible) {
            QString window_backend;
            QString window_warning;
            const QImage image = CaptureWindow(widget, &window_backend, &window_warning);
            if (!image.isNull()) {
                painter.drawImage(widget->geometry().topLeft() - bounds.topLeft(), image);
            }
            windows.append(QJsonObject{{QStringLiteral("id"), Id(widget)}, {QStringLiteral("class"), QString::fromUtf8(widget->metaObject()->className())},
                                       {QStringLiteral("geometry"), QJsonArray{widget->geometry().x(), widget->geometry().y(), widget->geometry().width(), widget->geometry().height()}},
                                       {QStringLiteral("backend"), window_backend}, {QStringLiteral("uniform"), Uniform(image)},
                                       {QStringLiteral("warning"), window_warning}});
        }
        painter.end();
        backend = QStringLiteral("composite");
    } else if (auto* widget = qobject_cast<QWidget*>(Resolve(target))) {
        target_id = Id(widget);
        // Capture the already-native top level and crop, so a child never acquires a native
        // window handle as a side effect of being photographed.
        QWidget* window = widget->window();
        capture = CropToWidget(CaptureWindow(window, &backend, &warning), widget, window);
    } else {
        return Error(QStringLiteral("unknown_target"), QStringLiteral("Select a widget/window from ui.tree, active, all, or screen"));
    }
    if (capture.isNull() || !capture.save(path, "PNG")) {
        return Error(QStringLiteral("capture_failed"), QStringLiteral("Capture unavailable on this display backend or output path is not writable"));
    }
    QJsonObject result{{QStringLiteral("path"), path}, {QStringLiteral("width"), capture.width()}, {QStringLiteral("height"), capture.height()},
                       {QStringLiteral("backend"), backend}, {QStringLiteral("target"), target_id}, {QStringLiteral("uniform"), Uniform(capture)}};
    if (!warning.isEmpty()) {
        result.insert(QStringLiteral("warning"), warning);
    }
    if (!windows.isEmpty()) {
        result.insert(QStringLiteral("windows"), windows);
    }
    return Success(result);
}

QJsonObject ControlServer::Trigger(QObject* target) {
    if (auto* shortcut = qobject_cast<QShortcut*>(target)) {
        auto* owner = WidgetFor(shortcut->parent());
        if (!shortcut->isEnabled() || shortcut->key().isEmpty() || (owner && !owner->isVisible())) {
            return Error(QStringLiteral("unavailable_shortcut"), QStringLiteral("Shortcut is disabled, unbound, or its window is hidden"));
        }
        emit shortcut->activated();
        return Success(QJsonObject{{QStringLiteral("path"), QStringLiteral("shortcut")}});
    }
    auto* action = qobject_cast<QAction*>(target);
    if (!action || !action->isEnabled() || !action->isVisible() || action->isSeparator()) {
        return Error(QStringLiteral("unavailable_action"), QStringLiteral("Target must be an enabled, visible action or shortcut"));
    }
    if (QMenu* popup = VisiblePopupMenuFor(action)) {
        // Follow the keyboard path: QMenu hides itself (and parent menus) before activating the
        // action, exactly as a user's Return key does, and it refuses disabled items.
        QPointer<QMenu> menu{popup};
        popup->setActiveAction(action);
        QKeyEvent press{QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier};
        QApplication::sendEvent(popup, &press);
        if (menu) {
            QKeyEvent release{QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier};
            QApplication::sendEvent(menu, &release);
        }
        return Success(QJsonObject{{QStringLiteral("path"), QStringLiteral("popup_key")}});
    }
    if (auto* menu = action->menu()) {
        for (auto* associated : action->associatedObjects()) {
            if (auto* bar = qobject_cast<QMenuBar*>(associated); bar && bar->isVisible()) {
                bar->setActiveAction(action); // opens the menu the way a click on the title does
                return Success(QJsonObject{{QStringLiteral("path"), QStringLiteral("menubar")}});
            }
        }
        auto* owner = WidgetFor(action->parent());
        menu->popup(owner && owner->isVisible() ? owner->mapToGlobal(QPoint{0, owner->height()}) : QCursor::pos());
        return Success(QJsonObject{{QStringLiteral("path"), QStringLiteral("popup")}});
    }
    action->trigger();
    return Success(QJsonObject{{QStringLiteral("path"), QStringLiteral("trigger")}});
}

QJsonObject ControlServer::Dispatch(const QString& method, const QJsonObject& params) {
    if (method == QStringLiteral("ping")) {
        return Success(QJsonObject{{QStringLiteral("protocol"), 1}, {QStringLiteral("pid"), QCoreApplication::applicationPid()}, {QStringLiteral("uptime_ms"), elapsed.elapsed()}, {QStringLiteral("ready"), static_cast<bool>(handler)},
                                   {QStringLiteral("modal"), QApplication::activeModalWidget() != nullptr}, {QStringLiteral("popup"), QApplication::activePopupWidget() != nullptr},
                                   {QStringLiteral("active_jobs"), active_jobs}, {QStringLiteral("background"), BackgroundMode()}});
    }
    if (method == QStringLiteral("schema")) {
        return Success(QJsonObject{{QStringLiteral("protocol"), 1},
            {QStringLiteral("methods"), QJsonArray::fromStringList({
                QStringLiteral("ping"), QStringLiteral("status"), QStringLiteral("schema"), QStringLiteral("ui.tree"), QStringLiteral("ui.actions"),
                QStringLiteral("ui.trigger"), QStringLiteral("ui.click"), QStringLiteral("ui.set"), QStringLiteral("ui.key"), QStringLiteral("ui.context"),
                QStringLiteral("ui.items"), QStringLiteral("ui.select"), QStringLiteral("ui.close"), QStringLiteral("ui.screenshot"), QStringLiteral("jobs.list"),
                QStringLiteral("jobs.get"), QStringLiteral("settings.list"), QStringLiteral("settings.get"), QStringLiteral("settings.set"), QStringLiteral("settings.save"),
                QStringLiteral("game.load"), QStringLiteral("game.pause"), QStringLiteral("game.resume"), QStringLiteral("game.stop"), QStringLiteral("input.schema"),
                QStringLiteral("input.state"), QStringLiteral("input.button"), QStringLiteral("input.stick"), QStringLiteral("input.motion"), QStringLiteral("input.touch"),
                QStringLiteral("input.trigger"), QStringLiteral("input.ring"),
                QStringLiteral("input.reset"), QStringLiteral("log.filter"),
                QStringLiteral("recording.start"), QStringLiteral("recording.stop"), QStringLiteral("recording.status"),
                QStringLiteral("tracepoint.arm"), QStringLiteral("tracepoint.status"), QStringLiteral("tracepoint.cancel"),
                QStringLiteral("macro.list"), QStringLiteral("macro.inspect"), QStringLiteral("macro.record"),
                QStringLiteral("macro.play"), QStringLiteral("macro.record_from"), QStringLiteral("macro.stop"),
                QStringLiteral("macro.marker"), QStringLiteral("macro.bind"), QStringLiteral("macro.shortcuts"),
                QStringLiteral("macro.status")})},
            {QStringLiteral("params"), QJsonObject{
                {QStringLiteral("tracepoint.arm"), QStringLiteral("{address:u64 string, registers:[x0..x30|sp|pc|pstate], memory_address?:u64 string OR memory_register?:register, memory_offset?:-1048576..1048576=0, memory_size?:1..4096=64 when memory requested, timeout_ms?:1..600000=5000}; one shot; A64 on x86-64 Dynarmic; GDB disabled; automatically resumes")},
                {QStringLiteral("tracepoint.status"), QStringLiteral("{} -> id, state, registers, memory.hex/error; only latest result retained; memory is CPU-visible, not globally atomic")},
                {QStringLiteral("tracepoint.cancel"), QStringLiteral("{} -> disarm pending tracepoint; idempotent; never suspends the guest")},
                {QStringLiteral("macro.list"), QStringLiteral("{} -> directory, macros, errors")},
                {QStringLiteral("macro.inspect"), QStringLiteral("{name|path} -> full macro document and path")},
                {QStringLiteral("macro.record"), QStringLiteral("{name, player?:0..9=0, path?:absolute, countdown_ms?:3000}; new named recording, no overwrite")},
                {QStringLiteral("macro.play"), QStringLiteral("{name|path, player?:0..9}; immediately replay saved inputs; requires matching initial game state")},
                {QStringLiteral("macro.record_from"), QStringLiteral("{name|path, marker, new_name, output_path?:absolute, player?:0..9, countdown_ms?:3000}; replay prefix blue then record new tail red")},
                {QStringLiteral("macro.stop"), QStringLiteral("{} -> finalize recording or stop playback and release its inputs")},
                {QStringLiteral("macro.marker"), QStringLiteral("{name?:string}; mark current recording position")},
                {QStringLiteral("macro.bind"), QStringLiteral("{name|path, keyboard:string, gamepad:string}; bind named playback shortcuts, empty clears")},
                {QStringLiteral("macro.shortcuts"), QStringLiteral("{marker_keyboard?:string, marker_gamepad?:string}; read/update marker shortcuts")},
                {QStringLiteral("macro.status"), QStringLiteral("{} -> state, cue, name, path, player, frame, duration_frames, countdown_remaining, paused, error")},
                {QStringLiteral("recording.start"), QStringLiteral("{path:absolute .gif, fps?:1..30=10, width?:160..1280=640, duration_ms?:100..120000=30000}; no overwrite; requires running loaded game")},
                {QStringLiteral("recording.stop"), QStringLiteral("{} -> finalize GIF; idempotent")},
                {QStringLiteral("recording.status"), QStringLiteral("{} -> state, path, frames, dropped_frames, elapsed_ms, duration_ms, fps, width, height, error")},
                {QStringLiteral("ui.tree"), QStringLiteral("{target?, visible_only?:bool, class?:string, brief?:bool} -> QWidget, QAction and QShortcut objects; brief omits properties")},
                {QStringLiteral("ui.actions"), QStringLiteral("same filters as ui.tree; QAction and QShortcut (hotkey-only commands) objects only")},
                {QStringLiteral("ui.trigger"), QStringLiteral("{target} QAction or QShortcut. Items of an open popup menu follow the keyboard path (menu closes first); menubar menus open; result.path reports which")},
                {QStringLiteral("ui.click"), QStringLiteral("{target, x?, y?} QAbstractButton::click, otherwise a synthetic left click on the widget (viewport for scroll areas)")},
                {QStringLiteral("ui.set"), QStringLiteral("{target, property, value} scalar Q_PROPERTY only; refuses enabled, objectName and QAction.checked (use ui.trigger)")},
                {QStringLiteral("ui.key"), QStringLiteral("{target, key, text?} one chord in PortableText (Return, Ctrl+P); reaches shortcuts, menus and dialogs")},
                {QStringLiteral("ui.context"), QStringLiteral("{target, x?, y?, row?, column?, parents?} opens a context menu; then list ui.actions and ui.trigger an item")},
                {QStringLiteral("ui.items"), QStringLiteral("{target, row?, column?, parents?, offset?, limit?} rows of an item view; read-only, allowed during modals")},
                {QStringLiteral("ui.select"), QStringLiteral("{target, row, column?, parents?, check_state?, value?, expand?, activate?}")},
                {QStringLiteral("ui.close"), QStringLiteral("{target} closes a window, dialog or popup menu")},
                {QStringLiteral("ui.screenshot"), QStringLiteral("{path, target?} target active (modal, popup, focused, else visible main window), all (every visible window composited), screen, id or objectName; children are cropped from their window. Result: backend x11-window|screen-window|widget-render|screen, uniform (single colour, i.e. no real pixels), warning")},
                {QStringLiteral("jobs.get"), QStringLiteral("{job} -> state queued|running|done|error and the final response")}}},
            {QStringLiteral("selectors"), QStringLiteral("Use a session id from ui.tree/actions, a unique objectName, or active. Ids are never reused; rediscover destroyed dialogs.")},
            {QStringLiteral("mutations"), QStringLiteral("UI/game operations return accepted/job. Inspect jobs.get with {job:id}; running jobs can be waiting in an interactive modal dialog, and later jobs run while it is open so they can close it.")},
            {QStringLiteral("errors"), QJsonArray::fromStringList({
                QStringLiteral("invalid_json"), QStringLiteral("invalid_request"), QStringLiteral("request_too_large"), QStringLiteral("response_too_large"),
                QStringLiteral("unknown_method"), QStringLiteral("unknown_target"), QStringLiteral("wrong_type"), QStringLiteral("invalid_params"),
                QStringLiteral("invalid_index"), QStringLiteral("invalid_value"), QStringLiteral("invalid_property"), QStringLiteral("invalid_path"),
                QStringLiteral("invalid_key"), QStringLiteral("read_only"), QStringLiteral("disabled"), QStringLiteral("unavailable_action"),
                QStringLiteral("unavailable_shortcut"), QStringLiteral("modal_active"), QStringLiteral("destroyed"), QStringLiteral("capture_failed"),
                QStringLiteral("busy"), QStringLiteral("unknown_job"), QStringLiteral("not_ready"), QStringLiteral("exception")})},
            {QStringLiteral("limits"), QJsonObject{{QStringLiteral("request_bytes"), static_cast<qint64>(MaxRequest)}, {QStringLiteral("response_bytes"), static_cast<qint64>(MaxResponse)},
                                                   {QStringLiteral("connections"), MaxConnections}, {QStringLiteral("idle_timeout_ms"), idle_timeout_ms},
                                                   {QStringLiteral("active_jobs"), MaxActiveJobs}, {QStringLiteral("job_history"), MaxJobHistory},
                                                   {QStringLiteral("requests_per_read"), RequestsPerRead}}}});
    }
    if (method == QStringLiteral("jobs.list")) {
        QJsonArray result;
        for (const auto& job : jobs) {
            result.append(job);
        }
        return Success(result);
    }
    if (method == QStringLiteral("jobs.get")) {
        bool ok{};
        const auto id = params.value(QStringLiteral("job")).toString().toULongLong(&ok);
        return ok && jobs.contains(id) ? Success(jobs.value(id)) : Error(QStringLiteral("unknown_job"), QStringLiteral("Job was not found or has expired"));
    }
    if (method == QStringLiteral("ui.tree") || method == QStringLiteral("ui.actions")) {
        QJsonArray result;
        const auto target = params.value(QStringLiteral("target")).toString();
        const bool visible_only = params.value(QStringLiteral("visible_only")).toBool();
        const bool brief = params.value(QStringLiteral("brief")).toBool();
        const auto class_filter = params.value(QStringLiteral("class")).toString();
        QObject* root = target.isEmpty() ? nullptr : Resolve(target);
        if (!target.isEmpty() && !root) {
            return Error(QStringLiteral("unknown_target"), QStringLiteral("Target is absent or ambiguous"));
        }
        for (auto* object : Objects()) {
            const bool command = qobject_cast<QAction*>(object) || qobject_cast<QShortcut*>(object);
            if (method == QStringLiteral("ui.actions") && !command) {
                continue;
            }
            if (!class_filter.isEmpty() && class_filter != QString::fromUtf8(object->metaObject()->className())) {
                continue;
            }
            if (root) {
                auto* parent = object;
                while (parent && parent != root) {
                    parent = parent->parent();
                }
                if (!parent) {
                    continue;
                }
            }
            if (visible_only) {
                if (auto* widget = qobject_cast<QWidget*>(object); widget && !widget->isVisible()) {
                    continue;
                }
                if (auto* action = qobject_cast<QAction*>(object); action && !action->isVisible()) {
                    continue;
                }
                if (auto* shortcut = qobject_cast<QShortcut*>(object)) {
                    auto* owner = WidgetFor(shortcut->parent());
                    if (owner && !owner->isVisible()) {
                        continue;
                    }
                }
            }
            result.append(Describe(object, brief));
        }
        return Success(result);
    }
    if (method == QStringLiteral("ui.screenshot")) {
        return Screenshot(params);
    }
    if (method.startsWith(QStringLiteral("ui."))) {
        QPointer<QObject> target = Resolve(params.value(QStringLiteral("target")).toString());
        if (!target) {
            return Error(QStringLiteral("unknown_target"), QStringLiteral("Target is absent or ambiguous; rediscover ui.tree/actions"));
        }
        if (method == QStringLiteral("ui.items")) {
            auto* view = qobject_cast<QAbstractItemView*>(target);
            if (!view || !view->model()) {
                return Error(QStringLiteral("wrong_type"), QStringLiteral("Target must be an item view"));
            }
            QJsonArray items;
            QModelIndex parent = view->rootIndex();
            if (params.contains(QStringLiteral("row"))) {
                parent = Index(view, params);
                if (!parent.isValid()) {
                    return Error(QStringLiteral("invalid_index"), QStringLiteral("Invalid parent row/column"));
                }
            }
            const auto offset_value = params.value(QStringLiteral("offset")).isUndefined() ? QJsonValue{0} : params.value(QStringLiteral("offset"));
            const auto limit_value = params.value(QStringLiteral("limit")).isUndefined() ? QJsonValue{100} : params.value(QStringLiteral("limit"));
            if (!Integer(offset_value, 0, 1000000) || !Integer(limit_value, 1, 500)) {
                return Error(QStringLiteral("invalid_params"), QStringLiteral("offset must be 0..1000000, limit 1..500"));
            }
            for (int row = offset_value.toInt(); row < std::min(view->model()->rowCount(parent), offset_value.toInt() + limit_value.toInt()); ++row) {
                QJsonArray columns;
                for (int col = 0; col < std::min(view->model()->columnCount(parent), 64); ++col) {
                    const auto index = view->model()->index(row, col, parent);
                    columns.append(QJsonObject{{QStringLiteral("text"), index.data().toString()}, {QStringLiteral("check_state"), QJsonValue::fromVariant(index.data(Qt::CheckStateRole))}, {QStringLiteral("flags"), static_cast<int>(index.flags())}});
                }
                items.append(QJsonObject{{QStringLiteral("row"), row}, {QStringLiteral("columns"), columns}, {QStringLiteral("children"), view->model()->hasChildren(view->model()->index(row, 0, parent))}});
            }
            return Success(QJsonObject{{QStringLiteral("rows"), view->model()->rowCount(parent)}, {QStringLiteral("items"), items}});
        }
        if (!Reachable(target)) {
            return Error(QStringLiteral("modal_active"), QStringLiteral("Target is blocked by a modal dialog"));
        }
        if (auto* widget = qobject_cast<QWidget*>(target); widget && !widget->isEnabled()) {
            return Error(QStringLiteral("disabled"), QStringLiteral("Target is disabled"));
        }
        return Enqueue(method, [this, target, method, params] {
            if (!target) {
                return Error(QStringLiteral("destroyed"), QStringLiteral("Target was destroyed before the operation began"));
            }
            if (!Reachable(target)) {
                return Error(QStringLiteral("modal_active"), QStringLiteral("Target is now blocked by a modal dialog"));
            }
            auto* widget = qobject_cast<QWidget*>(target);
            if (widget && !widget->isEnabled()) {
                return Error(QStringLiteral("disabled"), QStringLiteral("Target is disabled"));
            }
            if (method == QStringLiteral("ui.trigger")) {
                return Trigger(target);
            } else if (method == QStringLiteral("ui.set")) {
                const auto name = params.value(QStringLiteral("property")).toString().toUtf8();
                const int index = target->metaObject()->indexOfProperty(name.constData());
                if (index < 0 || !params.contains(QStringLiteral("value"))) {
                    return Error(QStringLiteral("invalid_property"), QStringLiteral("A known property and value are required"));
                }
                const auto property = target->metaObject()->property(index);
                // Do not let automation bypass native availability or change object identity.
                if (!property.isWritable() || !Scalar(property) || name == "enabled" || name == "objectName") {
                    return Error(QStringLiteral("read_only"), QStringLiteral("Property is not an editable scalar option"));
                }
                if (qobject_cast<QAction*>(target) && name == "checked") {
                    // setChecked only emits toggled; the application's handlers listen to triggered.
                    return Error(QStringLiteral("read_only"), QStringLiteral("Use ui.trigger to toggle a checkable action"));
                }
                const auto json = params.value(QStringLiteral("value"));
                QVariant value = json.toVariant();
                const auto type = property.metaType();
                if ((type == QMetaType::fromType<bool>() && !json.isBool()) ||
                    (type == QMetaType::fromType<QString>() && !json.isString()) ||
                    ((property.isEnumType() || type == QMetaType::fromType<int>()) && !Integer(json, std::numeric_limits<int>::min(), std::numeric_limits<int>::max())) ||
                    (type == QMetaType::fromType<uint>() && (!json.isDouble() || json.toDouble() < 0 || json.toDouble() > std::numeric_limits<unsigned>::max() || std::trunc(json.toDouble()) != json.toDouble())) ||
                    ((type == QMetaType::fromType<float>() || type == QMetaType::fromType<double>()) && (!json.isDouble() || !std::isfinite(json.toDouble())))) {
                    return Error(QStringLiteral("invalid_value"), QStringLiteral("Value does not match the discovered property type"));
                }
                if (property.isEnumType() && !property.enumerator().isFlag() && !property.enumerator().valueToKey(json.toInt())) {
                    return Error(QStringLiteral("invalid_value"), QStringLiteral("Unknown enumeration value"));
                }
                if (!value.convert(type) || !property.write(target, value)) {
                    return Error(QStringLiteral("invalid_value"), QStringLiteral("Property rejected the value"));
                }
                if (auto* line = qobject_cast<QLineEdit*>(target); line && name == "text") {
                    emit line->textEdited(line->text());
                    emit line->editingFinished();
                }
                return Success(QJsonObject{{QStringLiteral("value"), QJsonValue::fromVariant(property.read(target))}});
            } else if (method == QStringLiteral("ui.click")) {
                if (auto* button = qobject_cast<QAbstractButton*>(target)) {
                    button->click();
                } else if (widget) {
                    QPointer<QWidget> receiver = EventReceiver(widget);
                    const QPointF position{params.value(QStringLiteral("x")).toDouble(receiver->width() / 2.0), params.value(QStringLiteral("y")).toDouble(receiver->height() / 2.0)};
                    const QPointF global{receiver->mapToGlobal(position.toPoint())};
                    QMouseEvent press{QEvent::MouseButtonPress, position, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier};
                    QMouseEvent release{QEvent::MouseButtonRelease, position, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier};
                    QApplication::sendEvent(receiver, &press);
                    if (receiver) {
                        QApplication::sendEvent(receiver, &release);
                    }
                } else {
                    return Error(QStringLiteral("wrong_type"), QStringLiteral("Target must be a widget"));
                }
            } else if (method == QStringLiteral("ui.key")) {
                const auto sequence = QKeySequence::fromString(params.value(QStringLiteral("key")).toString(), QKeySequence::PortableText);
                if (!widget || sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown) {
                    return Error(QStringLiteral("invalid_key"), QStringLiteral("Expected one Qt key chord, e.g. Return or Ctrl+A"));
                }
                const auto key = sequence[0];
                const auto text = params.value(QStringLiteral("text")).toString();
                QPointer<QWidget> receiver = widget;
                widget->setFocus();
                QKeyEvent press{QEvent::KeyPress, key.key(), key.keyboardModifiers(), text};
                QKeyEvent release{QEvent::KeyRelease, key.key(), key.keyboardModifiers(), text};
                QApplication::sendEvent(receiver, &press);
                if (receiver) {
                    QApplication::sendEvent(receiver, &release);
                }
            } else if (method == QStringLiteral("ui.context")) {
                if (!widget) {
                    return Error(QStringLiteral("wrong_type"), QStringLiteral("Target must be a widget"));
                }
                QWidget* receiver = EventReceiver(widget);
                QPoint position{params.value(QStringLiteral("x")).toInt(receiver->width() / 2), params.value(QStringLiteral("y")).toInt(receiver->height() / 2)};
                if (auto* view = qobject_cast<QAbstractItemView*>(target); view && params.contains(QStringLiteral("row"))) {
                    const auto index = Index(view, params);
                    if (!index.isValid()) {
                        return Error(QStringLiteral("invalid_index"), QStringLiteral("Invalid model row/column"));
                    }
                    view->scrollTo(index);
                    position = view->visualRect(index).center();
                }
                QContextMenuEvent event{QContextMenuEvent::Mouse, position, receiver->mapToGlobal(position)};
                QApplication::sendEvent(receiver, &event);
            } else if (method == QStringLiteral("ui.select")) {
                auto* view = qobject_cast<QAbstractItemView*>(target);
                if (!view) {
                    return Error(QStringLiteral("wrong_type"), QStringLiteral("Target must be an item view"));
                }
                const auto index = Index(view, params);
                if (!index.isValid() || !(index.flags() & Qt::ItemIsEnabled)) {
                    return Error(QStringLiteral("invalid_index"), QStringLiteral("Invalid or disabled model index"));
                }
                view->setCurrentIndex(index);
                view->scrollTo(index);
                if (params.contains(QStringLiteral("check_state"))) {
                    const auto checked = params.value(QStringLiteral("check_state"));
                    if (!Integer(checked, 0, 2) || !(index.flags() & Qt::ItemIsUserCheckable) || !view->model()->setData(index, checked.toInt(), Qt::CheckStateRole)) {
                        return Error(QStringLiteral("invalid_value"), QStringLiteral("Item cannot take that check state"));
                    }
                }
                if (params.contains(QStringLiteral("value")) && !view->model()->setData(index, params.value(QStringLiteral("value")).toVariant(), Qt::EditRole)) {
                    return Error(QStringLiteral("invalid_value"), QStringLiteral("Item rejected the edit"));
                }
                if (params.value(QStringLiteral("expand")).toBool()) {
                    if (auto* tree = qobject_cast<QTreeView*>(view)) {
                        tree->expand(index);
                    }
                }
                if (params.value(QStringLiteral("activate")).toBool()) {
                    emit view->activated(index);
                    emit view->doubleClicked(index);
                }
            } else if (method == QStringLiteral("ui.close")) {
                if (!widget || !widget->isWindow()) {
                    return Error(QStringLiteral("wrong_type"), QStringLiteral("Target must be a window/dialog"));
                }
                widget->close();
            } else {
                return Error(QStringLiteral("unknown_method"), QStringLiteral("Unknown UI command"));
            }
            return Success();
        });
    }
    if (handler) {
        return handler(method, params);
    }
    return Error(QStringLiteral("not_ready"), QStringLiteral("Frontend is initializing; inspect ui.tree for startup dialogs"));
}
