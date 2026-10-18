// SPDX-License-Identifier: GPL-3.0-or-later
#include "yuzu/lan_online.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <memory>
#include <cerrno>
#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "common/settings.h"
#include "core/internal_network/nso_lan.h"
#include "qt_common/config/uisettings.h"

namespace LanOnline {
namespace {
class Runtime final : public QObject {
public:
    explicit Runtime(QObject* parent) : QObject{parent} {
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { Stop(); });
    }
    ~Runtime() override {
        Stop();
    }

    void Stop() {
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(2000)) {
                process.kill();
                process.waitForFinished(2000);
            }
        }
    }

    QProcess process;
    std::unique_ptr<QTemporaryDir> directory;
    bool published = false;
    bool starting = false;
    bool control_tls = false;
    u64 title = 0;
};

QPointer<Runtime> runtime;

[[maybe_unused]] QString LocateRuntime() {
    // Overrides support isolated development builds; normal packages are self-contained.
    const auto override = qEnvironmentVariable("EDEN_LAN_RUNTIME");
    if (!override.isEmpty()) {
        return override;
    }
    const auto application = QCoreApplication::applicationDirPath();
    for (const auto& candidate : {application + QStringLiteral("/eden-lan-runtime"),
                                  application + QStringLiteral("/../libexec/eden-lan-runtime")}) {
        if (QFileInfo(candidate).isExecutable()) {
            return candidate;
        }
    }
    return {};
}

[[maybe_unused]] QString LocateAssets() {
    const auto override = qEnvironmentVariable("EDEN_LAN_ASSETS");
    return override.isEmpty()
               ? QCoreApplication::applicationDirPath() + QStringLiteral("/../share/eden/lan-online/init")
               : override;
}

[[maybe_unused]] QString RuntimeFailure(const QString& directory) {
    QFile file{directory + QStringLiteral("/error.json")};
    if (file.open(QIODevice::ReadOnly)) {
        const auto bytes = file.read(4097);
        if (bytes.size() <= 4096) {
            const auto code = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("code")).toString();
            if (code == QStringLiteral("missing_assets")) {
                return QObject::tr("LAN Online support files are missing or invalid. Reinstall the complete private package.");
            }
            if (code == QStringLiteral("interface_unusable")) {
                return QObject::tr("LAN Online could not use the selected network interface. Check Network settings and your LAN connection.");
            }
            if (code == QStringLiteral("listen_failed")) {
                return QObject::tr("LAN Online could not open its local network sockets. Check local network permissions.");
            }
            if (code == QStringLiteral("runtime_dir_invalid")) {
                return QObject::tr("LAN Online could not use its private runtime directory.");
            }
            if (code == QStringLiteral("unsupported_title")) {
                return QObject::tr("This runtime package does not support the selected game.");
            }
            if (code == QStringLiteral("lease_conflict")) {
                return QObject::tr("A LAN Online instance identity conflicted. Restart Eden to obtain a new identity.");
            }
        }
    }
    return QObject::tr("LAN Online did not become ready. Check that the runtime package matches Eden and the selected network interface is available.");
}
} // namespace

QString Prepare(QWidget*, u64 title_id) {
    if (!Settings::values.lan_online.GetValue()) {
        return runtime && runtime->published
                   ? QObject::tr("Restart Eden to turn LAN Online off.")
                   : QString{};
    }
#ifndef __linux__
    return QObject::tr("LAN Online is currently available in the Linux preview.");
#else
    constexpr u64 SupportedTitle = 0x01006A800016E000;
    if (title_id != SupportedTitle) {
        return runtime && runtime->published
                   ? QObject::tr("Restart Eden before loading a different game after LAN Online.")
                   : QString{};
    }
    if (Settings::values.airplane_mode.GetValue()) {
        return QObject::tr("Turn Airplane Mode off to use LAN Online.");
    }
    if (qEnvironmentVariableIsSet("EDEN_NSO_LAN_CONFIG")) {
        return QObject::tr("An explicit online test configuration is active. Remove it before using automatic LAN Online.");
    }
    if (!runtime) {
        runtime = new Runtime(qApp);
    }
    if (runtime->starting) {
        return QObject::tr("LAN Online is still starting. Try again shortly.");
    }
    if (runtime->published) {
        if (runtime->control_tls != UISettings::values.lan_online_tls.GetValue()) {
            return QObject::tr("Restart Eden to change LAN lobby encryption.");
        }
        if (runtime->title != title_id) {
            return QObject::tr("Restart Eden before using LAN Online with a different game.");
        }
        return runtime->process.state() == QProcess::Running
                   ? QString{}
                   : QObject::tr("The LAN Online service stopped. Restart Eden to reconnect.");
    }
    const auto executable = LocateRuntime();
    if (executable.isEmpty() || !QFileInfo(executable).isExecutable()) {
        return QObject::tr("The LAN Online runtime is missing. Install the LAN Online package, or turn this mode off in Network settings.");
    }
    const auto assets = LocateAssets();
    const QStringList replies = {QStringLiteral("0x6e_7.bin"), QStringLiteral("0x6e_9.bin"),
                                 QStringLiteral("0x6e_10.bin"), QStringLiteral("0x6e_11.bin"),
                                 QStringLiteral("0x73_21.bin"), QStringLiteral("0x73_60.bin"),
                                 QStringLiteral("0x73_61.bin"), QStringLiteral("0x73_84.bin"),
                                 QStringLiteral("0x73_95.bin")};
    for (const auto& reply : replies) {
        const QFileInfo file{assets + QLatin1Char('/') + reply};
        if (!file.isFile() || !file.isReadable()) {
            return QObject::tr("LAN Online game support files are missing. Install the complete private LAN Online package.");
        }
    }
    const auto base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty()) {
        return QObject::tr("Eden could not find a private runtime directory for LAN Online.");
    }
    const auto root = base + QStringLiteral("/eden-lan");
    const auto encoded_root = QFile::encodeName(root);
    struct stat directory_stat{};
    // Never follow or chmod a pre-existing symlink. Each process gets its own 0700 child below.
    if ((mkdir(encoded_root.constData(), 0700) != 0 && errno != EEXIST) ||
        lstat(encoded_root.constData(), &directory_stat) != 0 ||
        !S_ISDIR(directory_stat.st_mode) || directory_stat.st_uid != getuid() ||
        (directory_stat.st_mode & 0777) != 0700) {
        return QObject::tr("Eden could not create its private LAN Online runtime directory.");
    }
    runtime->Stop();
    runtime->directory = std::make_unique<QTemporaryDir>(root + QStringLiteral("/i-XXXXXX"));
    if (!runtime->directory->isValid()) {
        return QObject::tr("Eden could not allocate a LAN Online instance.");
    }
    runtime->starting = true;
    const auto instance_dir = runtime->directory->path();
    QStringList arguments{QStringLiteral("--auto"), QStringLiteral("--parent-pid"),
                          QString::number(QCoreApplication::applicationPid()),
                          QStringLiteral("--runtime-dir"), instance_dir,
                          QStringLiteral("--assets"), assets, QStringLiteral("--title-id"),
                          QString::number(title_id, 16).rightJustified(16, QLatin1Char('0'))};
    const auto interface = Settings::values.network_interface.GetValue();
    runtime->control_tls = UISettings::values.lan_online_tls.GetValue();
    if (runtime->control_tls) {
        arguments << QStringLiteral("--control-tls");
    }
    if (!interface.empty()) {
        arguments << QStringLiteral("--interface") << QString::fromStdString(interface);
    }
    runtime->process.setProgram(executable);
    runtime->process.setArguments(arguments);
    runtime->process.setStandardOutputFile(QProcess::nullDevice());
    runtime->process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    runtime->process.start();
    if (!runtime->process.waitForStarted(2000)) {
        runtime->starting = false;
        return QObject::tr("Eden could not start the LAN Online runtime.");
    }
    QElapsedTimer deadline;
    deadline.start();
    while (deadline.elapsed() < 10000 && runtime->process.state() == QProcess::Running) {
        QFile ready{instance_dir + QStringLiteral("/ready.json")};
        if (ready.exists() && ready.open(QIODevice::ReadOnly)) {
            const auto bytes = ready.read(65537);
            if (bytes.size() <= 65536 && Network::NsoLan::InstallAutomaticConfiguration(
                    std::string_view{bytes.constData(), static_cast<std::size_t>(bytes.size())},
                    instance_dir.toStdString())) {
                runtime->published = true;
                runtime->title = title_id;
                runtime->starting = false;
                return {};
            }
            runtime->Stop();
            runtime->starting = false;
            return QObject::tr("The LAN Online runtime returned an incompatible configuration.");
        }
        // Keep repainting during bounded startup without accepting another boot request.
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
        runtime->process.waitForFinished(25);
    }
    runtime->Stop();
    runtime->starting = false;
    return RuntimeFailure(instance_dir);
#endif
}
} // namespace LanOnline
