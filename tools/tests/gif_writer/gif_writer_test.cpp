// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Encoder-side tests for GifWriter. This program checks the API contract (paths, overwrite
// protection, empty/cancelled recordings, write failures, no stray temporary files) and writes
// a set of GIFs together with their exact source frames and a manifest. validate_gifs.py then
// decodes the GIFs with Pillow and an independent LZW decoder and compares them to the sources.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
#ifdef Q_OS_LINUX
#include <sys/resource.h>
#include <csignal>
#endif

#include "yuzu/gif_writer.h"

namespace {

int total_checks = 0;
int failed_checks = 0;

void Check(bool ok, const char* expression, const char* file, int line,
           const QString& detail = {}) {
    ++total_checks;
    if (ok) {
        return;
    }
    ++failed_checks;
    std::fprintf(stderr, "FAIL %s:%d: %s %s\n", file, line, expression,
                 qPrintable(detail));
}

#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define CHECK_MSG(expression, detail) \
    Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (detail))

// ---------------------------------------------------------------------------------------------
// Synthetic frames
// ---------------------------------------------------------------------------------------------

/// Palette channel value as defined by the GIF writer's RGB332 palette (rounded to nearest).
constexpr int Level(int index, int levels) {
    return (index * 255 + levels / 2) / levels;
}

/// A colour that is exactly representable in the RGB332 palette, so it must round-trip exactly.
QRgb ExactColor(int r3, int g3, int b2) {
    return qRgb(Level(r3, 7), Level(g3, 7), Level(b2, 3));
}

void SetPixel(QImage& image, int x, int y, QRgb color) {
    auto* row = static_cast<QRgb*>(static_cast<void*>(image.scanLine(y)));
    row[x] = color;
}

void FillRect(QImage& image, int x0, int y0, int w, int h, QRgb color) {
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            SetPixel(image, x, y, color);
        }
    }
}

/// Animated, palette-exact frame: a smooth RGB gradient whose blue level changes per frame,
/// asymmetric corner markers to detect flips/rotations and a moving black block.
QImage PatternFrame(int w, int h, int index) {
    QImage image(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            SetPixel(image, x, y, ExactColor(x * 8 / w, y * 8 / h, index % 4));
        }
    }
    const int m = std::min({3, w / 4, h / 4});
    if (m >= 1) {
        FillRect(image, 0, 0, m, m, ExactColor(7, 0, 0));         // Top left: red.
        FillRect(image, w - m, 0, m, m, ExactColor(0, 7, 0));     // Top right: green.
        FillRect(image, 0, h - m, m, m, ExactColor(0, 0, 3));     // Bottom left: blue.
        FillRect(image, w - m, h - m, m, m, ExactColor(7, 7, 3)); // Bottom right: white.
    }
    if (w >= 16 && h >= 16) {
        FillRect(image, (index * 7) % (w - 4), h / 2, 4, 4, ExactColor(0, 0, 0));
    }
    return image;
}

/// Uniform random palette indices: worst case for LZW, exact after quantization.
QImage ExactNoiseFrame(int w, int h, std::mt19937& rng) {
    QImage image(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto r = static_cast<std::uint32_t>(rng());
            SetPixel(image, x, y,
                     ExactColor(static_cast<int>(r & 7), static_cast<int>((r >> 8) & 7),
                                static_cast<int>((r >> 16) & 3)));
        }
    }
    return image;
}

/// Uniform random 24-bit colours: must be quantized (and dithered) to within one palette step.
QImage RgbNoiseFrame(int w, int h, std::mt19937& rng) {
    QImage image(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto r = static_cast<std::uint32_t>(rng());
            SetPixel(image, x, y,
                     qRgb(static_cast<int>(r & 0xFF), static_cast<int>((r >> 8) & 0xFF),
                          static_cast<int>((r >> 16) & 0xFF)));
        }
    }
    return image;
}

QImage GradientFrame(int w, int h, int index) {
    QImage image(w, h, QImage::Format_RGB32);
    const int dx = std::max(w - 1, 1);
    const int dy = std::max(h - 1, 1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int b = ((x * 255 / dx) / 2 + (y * 255 / dy) / 2 + index * 40) & 0xFF;
            SetPixel(image, x, y, qRgb(x * 255 / dx, y * 255 / dy, b));
        }
    }
    return image;
}

QImage SolidFrame(int w, int h, QRgb color) {
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(color);
    return image;
}

/// The same pixels in an RGB32 image whose rows are padded with garbage; a writer that ignores
/// bytesPerLine() would pick up the padding.
QImage PaddedFrame(const QImage& source) {
    const qsizetype stride = source.bytesPerLine() + 20;
    auto* buffer = new std::vector<uchar>(static_cast<std::size_t>(stride * source.height()), 0xAB);
    for (int y = 0; y < source.height(); ++y) {
        std::memcpy(buffer->data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                    source.constScanLine(y), static_cast<std::size_t>(source.bytesPerLine()));
    }
    return QImage(
        buffer->data(), source.width(), source.height(), stride, QImage::Format_RGB32,
        [](void* info) { delete static_cast<std::vector<uchar>*>(info); }, buffer);
}

// ---------------------------------------------------------------------------------------------
// Recording helpers
// ---------------------------------------------------------------------------------------------

struct Output {
    QString root;
    QDir gifs;
    QDir raw;
    QJsonArray cases;
};

using FrameFactory = std::function<QImage(int)>;

/// Writes one recording through GifWriter, dumping every source frame as raw RGB24, and adds
/// it to the manifest. `exact` means decoded colours must equal the source; otherwise they must
/// be within one dithered palette step.
void RecordCase(Output& out, const QString& name, int w, int h, const std::vector<int>& delays,
                const FrameFactory& make_frame, bool exact, qint64 max_bytes = 0) {
    GifWriter writer;
    QString error;
    const QString gif_name = name + QStringLiteral(".gif");
    const bool opened = writer.Open(out.gifs.filePath(gif_name), w, h, error);
    CHECK_MSG(opened, name + QStringLiteral(": Open: ") + error);
    if (!opened) {
        return;
    }
    CHECK(writer.IsOpen());

    QJsonArray frame_files;
    QJsonArray delay_list;
    for (std::size_t i = 0; i < delays.size(); ++i) {
        const QImage frame = make_frame(static_cast<int>(i));

        const QImage rgb = frame.convertToFormat(QImage::Format_RGB32);
        QByteArray dump;
        dump.reserve(w * h * 3);
        for (int y = 0; y < h; ++y) {
            const auto* row =
                static_cast<const QRgb*>(static_cast<const void*>(rgb.constScanLine(y)));
            for (int x = 0; x < w; ++x) {
                dump.append(static_cast<char>(qRed(row[x])));
                dump.append(static_cast<char>(qGreen(row[x])));
                dump.append(static_cast<char>(qBlue(row[x])));
            }
        }
        const QString raw_name = QStringLiteral("%1_%2.rgb").arg(name).arg(static_cast<int>(i));
        QFile raw_file(out.raw.filePath(raw_name));
        const bool dumped = raw_file.open(QIODevice::WriteOnly) && raw_file.write(dump) == dump.size();
        CHECK_MSG(dumped, raw_name);
        frame_files.append(QString{QStringLiteral("raw/") + raw_name});
        delay_list.append(delays[i]);

        const bool added = writer.AddFrame(frame, delays[i], error);
        CHECK_MSG(added, name + QStringLiteral(": AddFrame: ") + error);
        if (!added) {
            writer.Cancel();
            return;
        }
        CHECK(error.isEmpty());
    }
    CHECK(writer.FrameCount() == static_cast<int>(delays.size()));

    const bool finished = writer.Finish(error);
    CHECK_MSG(finished, name + QStringLiteral(": Finish: ") + error);
    CHECK(!writer.IsOpen());
    if (!finished) {
        return;
    }

    QJsonObject entry;
    entry[QStringLiteral("name")] = name;
    entry[QStringLiteral("file")] = QString{QStringLiteral("gifs/") + gif_name};
    entry[QStringLiteral("width")] = w;
    entry[QStringLiteral("height")] = h;
    entry[QStringLiteral("delays_cs")] = delay_list;
    entry[QStringLiteral("frames")] = frame_files;
    entry[QStringLiteral("exact")] = exact;
    entry[QStringLiteral("max_bytes")] = max_bytes;
    out.cases.append(entry);
}

void RecordAllCases(Output& out) {
    // Animated, palette-exact; delays cover 1, the 8-bit boundary (255/256) and the 16-bit maximum.
    RecordCase(
        out, QStringLiteral("anim_exact"), 64, 48,
        {1, 2, 3, 5, 10, 25, 50, 100, 255, 256, 1000, 65535},
        [](int i) { return PatternFrame(64, 48, i); }, true);

    // Odd dimensions, and rows with padding in the source image.
    RecordCase(
        out, QStringLiteral("odd_exact"), 37, 23, {7, 1, 65535},
        [](int i) { return PatternFrame(37, 23, i); }, true);
    RecordCase(
        out, QStringLiteral("strided_exact"), 37, 23, {4, 6},
        [](int i) { return PaddedFrame(PatternFrame(37, 23, i)); }, true);

    // Input formats other than RGB32 are converted.
    RecordCase(
        out, QStringLiteral("format_rgb888"), 40, 30, {3, 3},
        [](int i) { return PatternFrame(40, 30, i).convertToFormat(QImage::Format_RGB888); }, true);
    RecordCase(
        out, QStringLiteral("format_argb32"), 40, 30, {3, 3},
        [](int i) { return PatternFrame(40, 30, i).convertToFormat(QImage::Format_ARGB32); }, true);

    // Degenerate and extreme shapes; widths above 255 and 65535 exercise the 16-bit fields.
    RecordCase(
        out, QStringLiteral("tiny_1x1"), 1, 1, {4}, [](int) { return PatternFrame(1, 1, 3); }, true);
    RecordCase(
        out, QStringLiteral("tiny_3x2"), 3, 2, {9, 8},
        [](int i) { return PatternFrame(3, 2, i); }, true);
    RecordCase(
        out, QStringLiteral("tall_5x300"), 5, 300, {5},
        [](int i) { return PatternFrame(5, 300, i); }, true);
    {
        auto rng = std::make_shared<std::mt19937>(0x57ADE);
        RecordCase(
            out, QStringLiteral("wide_65535x1"), 65535, 1, {9},
            [rng](int) { return ExactNoiseFrame(65535, 1, *rng); }, true);
    }

    // High-entropy frames force dictionary growth through every code width and repeated resets.
    {
        auto rng = std::make_shared<std::mt19937>(12345);
        RecordCase(
            out, QStringLiteral("noise_exact"), 160, 120, {2, 3, 4},
            [rng](int) { return ExactNoiseFrame(160, 120, *rng); }, true);
    }
    {
        auto rng = std::make_shared<std::mt19937>(67890);
        RecordCase(
            out, QStringLiteral("noise_rgb"), 160, 120, {1, 10, 100, 1000},
            [rng](int) { return RgbNoiseFrame(160, 120, *rng); }, false);
    }

    // Larger, smooth content that goes through ordered dithering.
    RecordCase(
        out, QStringLiteral("gradient_large"), 640, 360, {3, 3},
        [](int i) { return GradientFrame(640, 360, i); }, false);

    // Highly compressible content must actually compress.
    RecordCase(
        out, QStringLiteral("solid_256"), 256, 256, {1, 65535},
        [](int) { return SolidFrame(256, 256, qRgb(200, 100, 50)); }, false, 4096);

    // One-row exact noise of every length around the points where the code width changes (after
    // 254, ~766 and ~1790 codes) or the dictionary fills (~3838 codes), so the width of the end
    // code is exercised on both sides of each boundary.
    auto sweep = [&out](int first, int last) {
        for (int n = first; n <= last; ++n) {
            auto rng = std::make_shared<std::mt19937>(static_cast<std::uint32_t>(n) * 2654435761U);
            RecordCase(
                out, QStringLiteral("sweep_%1").arg(n, 4, 10, QLatin1Char('0')), n, 1, {2},
                [rng, n](int) { return ExactNoiseFrame(n, 1, *rng); }, true);
        }
    };
    sweep(240, 280);
    sweep(750, 810);
    sweep(1780, 1860);
    sweep(3930, 4040);
}

// ---------------------------------------------------------------------------------------------
// API behaviour
// ---------------------------------------------------------------------------------------------

QStringList ListDir(const QString& dir) {
    return QDir(dir).entryList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
}

QByteArray ReadAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool WriteFile(const QString& path, const QByteArray& content) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

bool IsGif(const QString& path) {
    return ReadAll(path).startsWith("GIF89a");
}

void TestUsageErrors() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QDir dir(tmp.path());
    const QImage frame = PatternFrame(16, 16, 0);
    QString error;

    GifWriter idle;
    CHECK(!idle.IsOpen());
    CHECK(!idle.AddFrame(frame, 5, error));
    CHECK(!error.isEmpty());
    CHECK(!idle.Finish(error));
    CHECK(!error.isEmpty());
    idle.Cancel(); // Must be safe when nothing is open.
    idle.Cancel();

    GifWriter w;
    // Invalid or unusable paths.
    CHECK(!w.Open(QString(), 16, 16, error));
    CHECK(!error.isEmpty());
    CHECK(!w.Open(dir.filePath(QStringLiteral("missing/sub/out.gif")), 16, 16, error));
    CHECK(!error.isEmpty());
    CHECK(!w.Open(tmp.path() + QStringLiteral("/"), 16, 16, error));
    CHECK(!w.IsOpen());
    CHECK(ListDir(tmp.path()).isEmpty());

    // Invalid dimensions.
    const QString ok_path = dir.filePath(QStringLiteral("size.gif"));
    for (const auto& size : std::vector<std::pair<int, int>>{
             {0, 10}, {10, 0}, {-1, 5}, {5, -1}, {65536, 5}, {5, 65536}, {0, 0}}) {
        error.clear();
        CHECK_MSG(!w.Open(ok_path, size.first, size.second, error),
                  QStringLiteral("%1x%2").arg(size.first).arg(size.second));
        CHECK(!error.isEmpty());
    }
    CHECK(ListDir(tmp.path()).isEmpty());

    // An invalid frame or delay does not spoil an open recording.
    const QString path = dir.filePath(QStringLiteral("usage.gif"));
    CHECK(w.Open(path, 16, 16, error));
    CHECK(error.isEmpty());
    CHECK(w.IsOpen());
    CHECK(!w.Open(dir.filePath(QStringLiteral("second.gif")), 16, 16, error));
    CHECK(!error.isEmpty());
    CHECK(w.IsOpen());
    CHECK(!w.AddFrame(QImage(), 5, error));
    CHECK(!error.isEmpty());
    CHECK(!w.AddFrame(PatternFrame(17, 16, 0), 5, error));
    CHECK(!w.AddFrame(PatternFrame(16, 15, 0), 5, error));
    for (const int delay : {0, -1, 65536, 1 << 20}) {
        error.clear();
        CHECK_MSG(!w.AddFrame(frame, delay, error), QString::number(delay));
        CHECK(!error.isEmpty());
    }
    CHECK(w.FrameCount() == 0);
    CHECK(w.AddFrame(frame, 1, error));
    CHECK(w.AddFrame(frame, 65535, error));
    CHECK(w.FrameCount() == 2);
    error = QStringLiteral("stale");
    CHECK(w.Finish(error));
    CHECK(error.isEmpty());
    CHECK(!w.IsOpen());
    CHECK(IsGif(path));
    CHECK(ListDir(tmp.path()) == QStringList{QStringLiteral("usage.gif")});

    // The writer is reusable after a successful recording, and after a failed Finish.
    const QString second = dir.filePath(QStringLiteral("second.gif"));
    CHECK(w.Open(second, 16, 16, error));
    CHECK(w.AddFrame(frame, 5, error));
    CHECK(w.Finish(error));
    CHECK(IsGif(second));
    CHECK(!w.Finish(error)); // Already finished.
}

void TestExistingDestination() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QDir dir(tmp.path());
    const QImage frame = PatternFrame(16, 16, 0);
    QString error;
    GifWriter w;

    // Open refuses an existing file and leaves it untouched.
    const QString existing = dir.filePath(QStringLiteral("existing.gif"));
    CHECK(WriteFile(existing, "KEEP"));
    CHECK(!w.Open(existing, 16, 16, error));
    CHECK(!error.isEmpty());
    CHECK(!w.IsOpen());
    CHECK(ReadAll(existing) == "KEEP");
    CHECK(ListDir(tmp.path()) == QStringList{QStringLiteral("existing.gif")});

    // ... an existing directory ...
    CHECK(dir.mkdir(QStringLiteral("folder.gif")));
    CHECK(!w.Open(dir.filePath(QStringLiteral("folder.gif")), 16, 16, error));

#ifdef Q_OS_UNIX
    // ... and a dangling symlink, which QFileInfo::exists() alone would not notice.
    const QString dangling = dir.filePath(QStringLiteral("dangling.gif"));
    CHECK(::symlink("does-not-exist", QFile::encodeName(dangling).constData()) == 0);
    CHECK(!w.Open(dangling, 16, 16, error));
    CHECK(!QFileInfo::exists(dangling));
    CHECK(QFileInfo(dangling).isSymLink());
#endif

    // A file that appears while recording is not overwritten by Finish, which also removes the
    // temporary file.
    const QString raced = dir.filePath(QStringLiteral("raced.gif"));
    CHECK(w.Open(raced, 16, 16, error));
    CHECK(w.AddFrame(frame, 5, error));
    CHECK(WriteFile(raced, "OTHER"));
    CHECK(!w.Finish(error));
    CHECK(!error.isEmpty());
    CHECK(!w.IsOpen());
    CHECK(ReadAll(raced) == "OTHER");

    QStringList expected{QStringLiteral("dangling.gif"), QStringLiteral("existing.gif"),
                         QStringLiteral("folder.gif"), QStringLiteral("raced.gif")};
#ifndef Q_OS_UNIX
    expected.removeAll(QStringLiteral("dangling.gif"));
#endif
    CHECK_MSG(ListDir(tmp.path()) == expected, ListDir(tmp.path()).join(QLatin1Char(',')));
}

void TestEmptyAndCancelled() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QDir dir(tmp.path());
    const QImage frame = PatternFrame(16, 16, 0);
    QString error;
    const QString path = dir.filePath(QStringLiteral("rec.gif"));

    // Nothing is created before Finish succeeds (only the temporary file exists while open).
    {
        GifWriter w;
        CHECK(w.Open(path, 16, 16, error));
        CHECK(!QFileInfo::exists(path));
        CHECK(w.AddFrame(frame, 5, error));
        CHECK(!QFileInfo::exists(path));
        w.Cancel();
        CHECK(!w.IsOpen());
        CHECK(w.FrameCount() == 0);
        CHECK(ListDir(tmp.path()).isEmpty());
    }
    // A recording without frames cancels itself and reports an error.
    {
        GifWriter w;
        CHECK(w.Open(path, 16, 16, error));
        CHECK(!w.Finish(error));
        CHECK(!error.isEmpty());
        CHECK(!w.IsOpen());
        CHECK(ListDir(tmp.path()).isEmpty());
        // The path is free again.
        CHECK(w.Open(path, 16, 16, error));
        CHECK(w.AddFrame(frame, 5, error));
        CHECK(w.Finish(error));
        CHECK(IsGif(path));
        QFile::remove(path);
    }
    // Destroying an unfinished writer discards the recording.
    {
        GifWriter w;
        CHECK(w.Open(path, 16, 16, error));
        CHECK(w.AddFrame(frame, 5, error));
        CHECK(w.AddFrame(frame, 5, error));
    }
    CHECK(ListDir(tmp.path()).isEmpty());
    // Cancel after a failed Finish (as the recorder does) is harmless.
    {
        GifWriter w;
        CHECK(w.Open(path, 16, 16, error));
        CHECK(!w.Finish(error));
        w.Cancel();
        CHECK(ListDir(tmp.path()).isEmpty());
    }
}

#ifdef Q_OS_UNIX
void TestUnwritableDirectory() {
    if (::geteuid() == 0) {
        return; // Permissions are not enforced for root.
    }
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    QString error;
    GifWriter w;
    CHECK(QFile::setPermissions(tmp.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    CHECK(!w.Open(QDir(tmp.path()).filePath(QStringLiteral("denied.gif")), 8, 8, error));
    CHECK(!error.isEmpty());
    CHECK(!w.IsOpen());
    CHECK(QFile::setPermissions(tmp.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                QFileDevice::ExeOwner));
    CHECK(ListDir(tmp.path()).isEmpty());
}
#endif

#ifdef Q_OS_LINUX
/// Fails a frame write for real: the file size limit makes write(2) return EFBIG.
void TestWriteFailure() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QDir dir(tmp.path());
    std::mt19937 rng(99);
    QString error;
    GifWriter w;
    const QString path = dir.filePath(QStringLiteral("full.gif"));
    CHECK(w.Open(path, 64, 64, error));
    std::signal(SIGXFSZ, SIG_IGN); // Report EFBIG from write(2) instead of killing the process.
    rlimit original{};
    CHECK(::getrlimit(RLIMIT_FSIZE, &original) == 0);
    rlimit limited = original;
    limited.rlim_cur = 2048; // The header is ~800 bytes; a noise frame needs several KB more.
    CHECK(::setrlimit(RLIMIT_FSIZE, &limited) == 0);

    const bool added = w.AddFrame(ExactNoiseFrame(64, 64, rng), 5, error);
    const QString first_error = error;
    CHECK(::setrlimit(RLIMIT_FSIZE, &original) == 0);
    CHECK(!added);
    CHECK_MSG(!first_error.isEmpty(), first_error);
    CHECK(!w.IsOpen());
    CHECK(ListDir(tmp.path()).isEmpty()); // The temporary file is discarded immediately.

    // The failure stays reported until Cancel(); the recording cannot be published.
    CHECK(!w.AddFrame(PatternFrame(64, 64, 0), 5, error));
    CHECK(error == first_error);
    CHECK(!w.Finish(error));
    CHECK(error == first_error);
    CHECK(ListDir(tmp.path()).isEmpty());
    CHECK(!QFileInfo::exists(path));

    // After Finish/Cancel the writer works again.
    CHECK(w.Open(path, 16, 16, error));
    CHECK(w.AddFrame(PatternFrame(16, 16, 0), 5, error));
    CHECK(w.Finish(error));
    CHECK(IsGif(path));
}
#endif

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <empty output directory>\n", argv[0]);
        return 2;
    }

    Output out;
    out.root = QDir::cleanPath(QString::fromLocal8Bit(argv[1]));
    const QDir root(out.root);
    if (root.exists() && !root.isEmpty()) {
        std::fprintf(stderr, "output directory %s must be empty\n", qPrintable(out.root));
        return 2;
    }
    out.gifs = QDir(root.filePath(QStringLiteral("gifs")));
    out.raw = QDir(root.filePath(QStringLiteral("raw")));
    if (!QDir().mkpath(out.gifs.path()) || !QDir().mkpath(out.raw.path())) {
        std::fprintf(stderr, "cannot create output directories under %s\n", qPrintable(out.root));
        return 2;
    }

    TestUsageErrors();
    TestExistingDestination();
    TestEmptyAndCancelled();
#ifdef Q_OS_UNIX
    TestUnwritableDirectory();
#endif
#ifdef Q_OS_LINUX
    TestWriteFailure();
#endif
    RecordAllCases(out);

    QJsonObject manifest;
    manifest[QStringLiteral("cases")] = out.cases;
    const bool wrote_manifest = WriteFile(root.filePath(QStringLiteral("manifest.json")),
                                          QJsonDocument(manifest).toJson(QJsonDocument::Indented));
    CHECK(wrote_manifest);

    std::printf("%d checks, %d failed, %d GIFs written\n", total_checks, failed_checks,
                static_cast<int>(out.cases.size()));
    return failed_checks == 0 ? 0 : 1;
}
