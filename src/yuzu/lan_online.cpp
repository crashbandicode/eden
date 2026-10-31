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
#include <QUuid>
#include <memory>
#include <cerrno>
#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "common/fs/path_util.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/internal_network/nso_lan.h"
#ifdef _WIN32
#include "core/internal_network/nso_lan_platform.h"
#endif
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"

namespace LanOnline {
namespace {
#ifdef _WIN32
// QTemporaryDir supplies its own POSIX-style ACL, including group/world entries, instead of
// preserving the private parent ACL. Create each new Windows instance with our native DACL.
class InstanceDirectory final {
public:
    explicit InstanceDirectory(const QString& root) {
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = root + QStringLiteral("/i-") +
                                   QUuid::createUuid().toString(QUuid::Id128).left(6);
            if (Network::NsoLan::CreatePrivateDirectory(
                    QDir::toNativeSeparators(candidate).toStdString())) {
                owned_path = candidate;
                return;
            }
            if (!QFileInfo::exists(candidate)) {
                return;
            }
        }
    }
    ~InstanceDirectory() {
        if (isValid()) {
            QDir{owned_path}.removeRecursively();
        }
    }
    bool isValid() const { return !owned_path.isEmpty(); }
    QString path() const { return owned_path; }

private:
    QString owned_path;
};
#else
using InstanceDirectory = QTemporaryDir;
#endif

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
#ifdef _WIN32
            // QProcess::terminate cannot signal a console-less child on Windows; the runtime
            // polls for this marker in its private directory and stops cleanly within 250 ms.
            if (directory && directory->isValid()) {
                QFile marker{directory->path() + QStringLiteral("/stop")};
                if (marker.open(QIODevice::WriteOnly)) {
                    marker.close();
                }
            }
#endif
            process.terminate();
            if (!process.waitForFinished(2000)) {
                process.kill();
                process.waitForFinished(2000);
            }
        }
    }

    QProcess process;
    std::unique_ptr<InstanceDirectory> directory;
    bool published = false;
    bool starting = false;
    bool control_tls = false;
    u64 title = 0;
    Common::UUID profile_uuid{};
};

QPointer<Runtime> runtime;

[[maybe_unused]] QString LocateRuntime() {
    // Overrides support isolated development builds; normal packages are self-contained.
    const auto override = qEnvironmentVariable("EDEN_LAN_RUNTIME");
    if (!override.isEmpty()) {
        return override;
    }
    const auto application = QCoreApplication::applicationDirPath();
#ifdef _WIN32
    const QStringList candidates{application + QStringLiteral("/eden-lan-runtime.exe")};
#else
    const QStringList candidates{application + QStringLiteral("/eden-lan-runtime"),
                                 application + QStringLiteral("/../libexec/eden-lan-runtime")};
#endif
    for (const auto& candidate : candidates) {
        if (QFileInfo(candidate).isExecutable()) {
            return candidate;
        }
    }
    return {};
}

[[maybe_unused]] QString LocateAssets() {
    const auto override = qEnvironmentVariable("EDEN_LAN_ASSETS");
    if (!override.isEmpty()) {
        return override;
    }
#ifdef _WIN32
    return QCoreApplication::applicationDirPath() + QStringLiteral("/lan-online/init");
#else
    return QCoreApplication::applicationDirPath() + QStringLiteral("/../share/eden/lan-online/init");
#endif
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
            if (code == QStringLiteral("profile_store_invalid")) {
                return QObject::tr("LAN Online rating profile storage is not accessible.");
            }
            if (code == QStringLiteral("profile_in_use")) {
                return QObject::tr("A LAN Online rating profile for this game is already in use. Close other sessions first.");
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
#if !defined(__linux__) && !defined(_WIN32)
    return QObject::tr("LAN Online is currently available in the Linux and Windows previews.");
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
    const auto uuid_opt = QtCommon::system->GetProfileManager().GetUser(
        static_cast<std::size_t>(Settings::values.current_user.GetValue()));
    if (!uuid_opt.has_value()) {
        return QObject::tr("No emulated user account is selected. Select a user in System settings.");
    }
    const Common::UUID uuid = *uuid_opt;
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
        if (runtime->profile_uuid != uuid) {
            return QObject::tr("Restart Eden to use LAN Online with a different user account.");
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
    const auto& eden_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir);
    const std::filesystem::path profile_dir =
        eden_dir / "lan-online" / uuid.FormattedString() /
        QString::number(title_id, 16).rightJustified(16, QLatin1Char('0')).toStdString();
    {
        std::error_code dir_ec;
        std::filesystem::create_directories(profile_dir.parent_path(), dir_ec);
        if (dir_ec) {
            return QObject::tr("Eden could not create the LAN Online rating profile directory.");
        }
    }
#ifdef _WIN32
    // Per-user profile data (%LOCALAPPDATA%), kept short because AF_UNIX paths are limited to
    // 107 bytes. The root gets a protected DACL (this account and SYSTEM only) and each instance
    // child receives the same DACL. An existing root is verified, never re-permissioned.
    const auto base = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"));
    if (base.isEmpty() || !QDir{base}.exists()) {
        return QObject::tr("Eden could not find a private runtime directory for LAN Online.");
    }
    const auto root = base + QStringLiteral("/eden-lan");
    if (!Network::NsoLan::EnsurePrivateDirectory(QDir::toNativeSeparators(root).toStdString())) {
        return QObject::tr("Eden could not create its private LAN Online runtime directory.");
    }
#else
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
#endif
    runtime->Stop();
#ifdef _WIN32
    runtime->directory = std::make_unique<InstanceDirectory>(root);
#else
    runtime->directory = std::make_unique<InstanceDirectory>(root + QStringLiteral("/i-XXXXXX"));
#endif
    if (!runtime->directory->isValid()) {
        return QObject::tr("Eden could not allocate a LAN Online instance.");
    }
#ifdef _WIN32
    // The runtime and the guest sockets use native paths. The longest node below the instance
    // directory is eln-<pid>-<seq>-<16 hex>.sock; the whole path must fit sun_path (107 bytes).
    const auto instance_dir = QDir::toNativeSeparators(runtime->directory->path());
    const auto assets_arg = QDir::toNativeSeparators(assets);
    if (!Network::NsoLan::IsPrivateDirectory(instance_dir.toStdString()) ||
        instance_dir.toUtf8().size() + 1 + 48 > 107) {
        runtime->directory.reset();
        return QObject::tr("Eden could not allocate a private LAN Online instance directory. Check the "
                           "profile path length and permissions.");
    }
#else
    const auto instance_dir = runtime->directory->path();
    const auto assets_arg = assets;
#endif
#ifdef _WIN32
    const auto profile_arg = QDir::toNativeSeparators(QString::fromStdWString(profile_dir.wstring()));
#else
    const auto profile_arg = QString::fromStdString(profile_dir.string());
#endif
    runtime->starting = true;
    QStringList arguments{QStringLiteral("--auto"), QStringLiteral("--parent-pid"),
                          QString::number(QCoreApplication::applicationPid()),
                          QStringLiteral("--runtime-dir"), instance_dir,
                          QStringLiteral("--assets"), assets_arg, QStringLiteral("--title-id"),
                          QString::number(title_id, 16).rightJustified(16, QLatin1Char('0')),
                          QStringLiteral("--profile-dir"),
                          profile_arg};
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
                runtime->profile_uuid = uuid;
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
