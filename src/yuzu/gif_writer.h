// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QString>

class QImage;
class QSaveFile;

/**
 * Streaming GIF89a writer.
 *
 * Frames are quantized to a fixed global RGB332 palette (with a light ordered dither), LZW
 * compressed and written one at a time, so memory use is bounded by a single frame. The file is
 * written through QSaveFile: nothing appears at the destination until Finish() succeeds, and
 * Cancel() (or destruction) discards the temporary file.
 *
 * Output layout: header, global palette (256 entries), infinite NETSCAPE2.0 loop, then for every
 * frame a Graphic Control Extension (disposal 1, no transparency) and a full-size image.
 *
 * An existing file at the destination is never replaced: Open() refuses it and Finish() checks
 * again immediately before committing. The final rename itself is not atomic with that check, so
 * a file created by another process in that instant could still be replaced.
 *
 * Not thread-safe and not reentrant. All calls, including destruction, must happen on one thread
 * (the dedicated recording worker); the object owns a QSaveFile and so is bound to that thread.
 */
class GifWriter {
    Q_DECLARE_TR_FUNCTIONS(GifWriter)

public:
    static constexpr int kMaxDimension = 65535;
    static constexpr int kMinDelayCs = 1;
    static constexpr int kMaxDelayCs = 65535;

    GifWriter();
    ~GifWriter();

    GifWriter(const GifWriter&) = delete;
    GifWriter& operator=(const GifWriter&) = delete;

    /// Starts a recording of fixed-size frames. Fails if the path already exists, the dimensions
    /// are outside 1..65535, the writer is already open, or the destination cannot be created.
    bool Open(const QString& path, int width, int height, QString& error);

    /// Appends one frame displayed for delay_cs hundredths of a second (1..65535). The image must
    /// be exactly the size given to Open(); formats other than RGB32 are converted. After a write
    /// failure the temporary file is discarded and the writer stays failed until Cancel().
    bool AddFrame(const QImage& image, int delay_cs, QString& error);

    /// Writes the trailer and publishes the file. A recording without frames is cancelled and
    /// reported as an error, leaving no file behind. The writer is closed afterwards either way.
    bool Finish(QString& error);

    /// Discards any in-progress output. Safe to call at any time, including when not open.
    void Cancel();

    [[nodiscard]] bool IsOpen() const;
    [[nodiscard]] int FrameCount() const;

private:
    /// Writes and flushes data; returns an error message, or an empty string on success.
    QString WriteBytes(const std::vector<std::uint8_t>& data);
    /// Discards the output after a write failure and remembers the message until Cancel().
    bool Fail(const QString& message, QString& error);

    std::unique_ptr<QSaveFile> file;
    QString path;
    QString failure;
    int width = 0;
    int height = 0;
    int frame_count = 0;
    std::vector<std::uint8_t> indexed; ///< One frame of palette indices, reused between frames.
    std::vector<std::uint8_t> encoded; ///< One encoded frame (extension, descriptor, LZW data).
};
