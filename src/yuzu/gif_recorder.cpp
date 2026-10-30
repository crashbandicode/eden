// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "yuzu/gif_recorder.h"

#include <algorithm>
#include <QApplication>
#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include "common/settings.h"
#include "core/core.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"
#include "qt_common/render/emu_thread.h"
#include "video_core/renderer_base.h"
#include "yuzu/bootmanager.h"
#include "yuzu/gif_writer.h"

struct GifRecorder::Encoder {
    GifWriter writer;
    QImage previous;
    int frames{};
    qint64 written_cs{};
    QString error;

    void Flush(qint64 end_ms) {
        if (previous.isNull() || !error.isEmpty()) return;
        // Quantize cumulative timestamps, avoiding drift at rates such as 30 fps.
        const auto end_cs = std::max(written_cs + 1, (end_ms + 5) / 10);
        if (writer.AddFrame(previous, static_cast<int>(std::min<qint64>(65535, end_cs - written_cs)), error)) {
            written_cs = end_cs;
            ++frames;
        }
        previous = {};
    }
};

GifRecorder::GifRecorder(GRenderWindow& window_, QObject* parent)
    : QObject(parent), window(window_), encoder(std::make_unique<Encoder>()) {
    worker = new QObject;
    worker->moveToThread(&worker_thread);
    connect(&worker_thread, &QThread::finished, worker, &QObject::deleteLater);
    worker_thread.start();
    timer.setTimerType(Qt::PreciseTimer);
    connect(&timer, &QTimer::timeout, this, &GifRecorder::Capture);
    duration_timer.setSingleShot(true);
    duration_timer.setTimerType(Qt::PreciseTimer);
    connect(&duration_timer, &QTimer::timeout, this, &GifRecorder::Stop);
}

GifRecorder::~GifRecorder() {
    Stop();
    QMetaObject::invokeMethod(worker, [this] { encoder.reset(); }, Qt::BlockingQueuedConnection);
    worker_thread.quit();
    worker_thread.wait();
}

bool GifRecorder::Start(const QString& output, int rate, int requested_width, int duration,
                        QString& error) {
    if (IsRecording()) {
        error = tr("A GIF recording is already active.");
        return false;
    }
    if (!QtCommon::system->IsPoweredOn() || QtCommon::system->IsShuttingDown() ||
        !QtCommon::emu_thread ||
        !QtCommon::emu_thread->IsRunning() || !window.IsLoadingComplete()) {
        error = tr("Start a game and wait for it to finish loading before recording.");
        return false;
    }
    if (Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Null) {
        error = tr("GIF recording requires a graphics renderer.");
        return false;
    }
    if (rate < 1 || rate > 30 || requested_width < 160 || requested_width > 1280 ||
        duration < 100 || duration > 120000 || !QFileInfo(output).isAbsolute() ||
        !output.endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive)) {
        error = tr("Use an absolute .gif path, 1–30 fps, width 160–1280, and duration 100–120000 ms.");
        return false;
    }
    const auto aspect_width = UISettings::CalculateWidth(720, Settings::values.aspect_ratio.GetValue());
    const int requested_height = std::max(1, static_cast<int>(requested_width * 720U / aspect_width));
    bool opened{};
    QMetaObject::invokeMethod(worker, [&] {
        encoder->previous = {};
        encoder->error.clear();
        encoder->written_cs = 0;
        encoder->frames = 0;
        opened = encoder->writer.Open(output, requested_width, requested_height, error);
    }, Qt::BlockingQueuedConnection);
    if (!opened) return false;
    path = output;
    width = requested_width;
    height = requested_height;
    fps = rate;
    duration_ms = duration;
    frames = dropped = 0;
    elapsed_ms = 0;
    failure.clear();
    busy = false;
    ++generation;
    state = QStringLiteral("recording");
    clock.start();
    timer.start(std::max(1, 1000 / fps));
    duration_timer.start(duration_ms);
    emit Changed();
    Capture();
    return true;
}

bool GifRecorder::IsRecording() const {
    return state == QStringLiteral("recording");
}

void GifRecorder::Capture() {
    if (!IsRecording()) return;
    if (clock.elapsed() >= duration_ms || !QtCommon::system->IsPoweredOn() ||
        QtCommon::system->IsShuttingDown()) {
        Stop();
        return;
    }
    if (!QtCommon::emu_thread || !QtCommon::emu_thread->IsRunning()) return;
    auto& renderer = QtCommon::system->Renderer();
    if (busy || renderer.IsScreenshotPending()) {
        ++dropped;
        return;
    }
    busy = true;
    const auto token = generation;
    auto image = std::make_shared<QImage>(width, height, QImage::Format_RGB32);
    if (image->isNull()) {
        failure = tr("Could not allocate the GIF capture buffer.");
        Stop();
        return;
    }
    const QPointer<GifRecorder> target(this);
    // The callback owns its pixel buffer even if recording stops or this QObject is destroyed.
    // QPointer is inspected only on the GUI thread, through the application context.
    const bool accepted = renderer.RequestScreenshot(image->bits(), [target, image, token](bool inverted) {
        QMetaObject::invokeMethod(qApp, [target, image, token, inverted] {
            if (!target || !target->IsRecording() || target->generation != token) return;
            auto* self = target.data();
            const auto timestamp = self->clock.elapsed();
            QMetaObject::invokeMethod(self->worker, [self, target, image, token, inverted, timestamp] {
                auto& enc = *self->encoder;
                enc.Flush(timestamp);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
                enc.previous = inverted ? image->flipped(Qt::Vertical) : *image;
#else
                enc.previous = inverted ? image->mirrored(false, true) : *image;
#endif
                const auto error = enc.error;
                QMetaObject::invokeMethod(qApp, [target, token, error] {
                    if (!target || !target->IsRecording() || target->generation != token) return;
                    target->busy = false;
                    ++target->frames;
                    if (!error.isEmpty()) {
                        target->failure = error;
                        target->Stop();
                    }
                }, Qt::QueuedConnection);
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }, Layout::DefaultFrameLayout(static_cast<u32>(width), static_cast<u32>(height)));
    if (!accepted) {
        busy = false;
        ++dropped;
    }
}

void GifRecorder::Stop() {
    if (!IsRecording()) return;
    timer.stop();
    duration_timer.stop();
    elapsed_ms = clock.elapsed();
    state = QStringLiteral("finished");
    ++generation;
    QMetaObject::invokeMethod(worker, [this] {
        encoder->Flush(elapsed_ms);
        if (!failure.isEmpty() || !encoder->error.isEmpty()) {
            if (failure.isEmpty()) failure = encoder->error;
            encoder->writer.Cancel();
        } else if (!encoder->writer.Finish(failure)) {
            encoder->writer.Cancel();
        }
        frames = encoder->frames;
        encoder->previous = {};
    }, Qt::BlockingQueuedConnection);
    if (!failure.isEmpty()) state = QStringLiteral("error");
    busy = false;
    emit Changed();
}

QJsonObject GifRecorder::Status() const {
    return {{QStringLiteral("state"), state}, {QStringLiteral("path"), path},
            {QStringLiteral("frames"), frames}, {QStringLiteral("dropped_frames"), dropped},
            {QStringLiteral("elapsed_ms"), IsRecording() ? clock.elapsed() : elapsed_ms},
            {QStringLiteral("duration_ms"), duration_ms}, {QStringLiteral("fps"), fps},
            {QStringLiteral("width"), width}, {QStringLiteral("height"), height},
            {QStringLiteral("error"), failure}};
}
