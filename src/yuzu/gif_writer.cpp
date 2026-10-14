// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <iterator>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>

#include "yuzu/gif_writer.h"

namespace {

// LZW parameters for GIF with an 8-bit minimum code size (256 palette entries).
constexpr std::uint8_t kMinCodeSize = 8;
constexpr std::uint32_t kClearCode = 1U << kMinCodeSize;
constexpr std::uint32_t kEndCode = kClearCode + 1;
constexpr std::uint32_t kFirstFreeCode = kClearCode + 2;
constexpr std::uint32_t kCodeLimit = 4096; // Codes are at most 12 bits wide.
constexpr unsigned kInitialCodeWidth = kMinCodeSize + 1;
constexpr unsigned kMaxCodeWidth = 12;
constexpr std::size_t kMaxBlockSize = 255;

// Open-addressing dictionary: at most kCodeLimit entries in twice as many slots.
constexpr std::size_t kTableSize = 8192;
constexpr std::uint32_t kEmptyKey = 0xFFFFFFFFU;

constexpr std::uint8_t kTrailer = 0x3B;
constexpr std::uint8_t kSignature[] = {'G', 'I', 'F', '8', '9', 'a'};

// Application extension "NETSCAPE2.0", sub-block 1 with a loop count of 0 (repeat forever).
constexpr std::uint8_t kLoopExtension[] = {0x21, 0xFF, 0x0B, 'N',  'E',  'T',  'S',
                                           'C',  'A',  'P',  'E',  '2',  '.',  '0',
                                           0x03, 0x01, 0x00, 0x00, 0x00};

void PutLe16(std::vector<std::uint8_t>& out, int value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

constexpr int PaletteLevel(int index, int levels) {
    return (index * 255 + levels / 2) / levels;
}

/// Global palette: index bits are RRRGGGBB, each channel expanded to the full 0..255 range.
void AppendPalette(std::vector<std::uint8_t>& out) {
    for (int i = 0; i < 256; ++i) {
        out.push_back(static_cast<std::uint8_t>(PaletteLevel((i >> 5) & 7, 7)));
        out.push_back(static_cast<std::uint8_t>(PaletteLevel((i >> 2) & 7, 7)));
        out.push_back(static_cast<std::uint8_t>(PaletteLevel(i & 3, 3)));
    }
}

constexpr std::uint8_t kBayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

/// For each of the 16 ordered-dither thresholds, maps a channel value to its already-positioned
/// bits of the palette index. The dither offset is at most a quarter of a quantization step, so
/// values that are exactly on the palette always keep their index.
struct QuantTables {
    std::array<std::uint8_t, 16 * 256> red;
    std::array<std::uint8_t, 16 * 256> green;
    std::array<std::uint8_t, 16 * 256> blue;

    QuantTables() {
        for (int k = 0; k < 16; ++k) {
            const double offset = ((k + 0.5) / 16.0 - 0.5) * 0.5;
            for (int v = 0; v < 256; ++v) {
                const auto slot = static_cast<std::size_t>(k * 256 + v);
                red[slot] = static_cast<std::uint8_t>(Quantize(v, 7, offset) << 5);
                green[slot] = static_cast<std::uint8_t>(Quantize(v, 7, offset) << 2);
                blue[slot] = static_cast<std::uint8_t>(Quantize(v, 3, offset));
            }
        }
    }

private:
    static int Quantize(int value, int levels, double offset) {
        const int level = static_cast<int>(value * levels / 255.0 + 0.5 + offset);
        return std::clamp(level, 0, levels);
    }
};

void QuantizeImage(const QImage& image, std::vector<std::uint8_t>& indices) {
    static const QuantTables tables;

    const int width = image.width();
    const int height = image.height();
    for (int y = 0; y < height; ++y) {
        const auto* src = static_cast<const QRgb*>(static_cast<const void*>(image.constScanLine(y)));
        std::uint8_t* dst = indices.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        const std::uint8_t* thresholds = kBayer4[y & 3];
        for (int x = 0; x < width; ++x) {
            const QRgb pixel = src[x];
            const auto base = static_cast<std::size_t>(thresholds[x & 3]) * 256;
            dst[x] = static_cast<std::uint8_t>(tables.red[base + static_cast<std::size_t>(qRed(pixel))] |
                                               tables.green[base + static_cast<std::size_t>(qGreen(pixel))] |
                                               tables.blue[base + static_cast<std::size_t>(qBlue(pixel))]);
        }
    }
}

/// Encodes one frame of palette indices as GIF-flavoured variable-width LZW and appends it,
/// including the minimum code size byte, data sub-blocks and block terminator, to `out`.
///
/// Code width follows the decoder's view of the dictionary, which lags the encoder by one entry:
/// after each emitted code the encoder adds an entry, and the width grows once the next free code
/// no longer fits. When the dictionary is full a clear code is sent and it starts over.
class LzwEncoder {
public:
    explicit LzwEncoder(std::vector<std::uint8_t>& output)
        : out{output}, keys(kTableSize, kEmptyKey), values(kTableSize, 0) {}

    void Encode(const std::uint8_t* pixels, std::size_t count) {
        out.push_back(kMinCodeSize);
        ResetDictionary();
        Emit(kClearCode);

        std::uint32_t prefix = pixels[0];
        for (std::size_t i = 1; i < count; ++i) {
            const std::uint32_t pixel = pixels[i];
            const std::uint32_t key = (prefix << 8) | pixel;
            std::size_t slot = Hash(key);
            while (keys[slot] != kEmptyKey && keys[slot] != key) {
                slot = (slot + 1) & (kTableSize - 1);
            }
            if (keys[slot] == key) {
                prefix = values[slot];
                continue;
            }

            Emit(prefix);
            if (next_code < kCodeLimit) {
                keys[slot] = key;
                values[slot] = static_cast<std::uint16_t>(next_code);
                AdvanceNextCode();
            } else {
                Emit(kClearCode);
                ResetDictionary();
            }
            prefix = pixel;
        }

        Emit(prefix);
        // A decoder adds an entry for the final code too, so account for it to keep the width of
        // the end code in step with the decoder. (This happens to produce the same bytes either
        // way, as the extra bit of a widened end code is always padding.)
        if (next_code < kCodeLimit) {
            AdvanceNextCode();
        }
        Emit(kEndCode);
        if (bit_count > 0) {
            PutByte(static_cast<std::uint8_t>(bit_buffer & 0xFF));
        }
        FlushBlock();
        out.push_back(0);
    }

private:
    static std::size_t Hash(std::uint32_t key) {
        return static_cast<std::size_t>((key * 0x9E3779B1U) >> 19); // Top 13 bits.
    }

    void ResetDictionary() {
        std::fill(keys.begin(), keys.end(), kEmptyKey);
        next_code = kFirstFreeCode;
        code_width = kInitialCodeWidth;
    }

    void AdvanceNextCode() {
        ++next_code;
        if (next_code > (1U << code_width) && code_width < kMaxCodeWidth) {
            ++code_width;
        }
    }

    void Emit(std::uint32_t code) {
        bit_buffer |= code << bit_count;
        bit_count += code_width;
        while (bit_count >= 8) {
            PutByte(static_cast<std::uint8_t>(bit_buffer & 0xFF));
            bit_buffer >>= 8;
            bit_count -= 8;
        }
    }

    void PutByte(std::uint8_t byte) {
        block[block_size++] = byte;
        if (block_size == kMaxBlockSize) {
            FlushBlock();
        }
    }

    void FlushBlock() {
        if (block_size == 0) {
            return;
        }
        out.push_back(static_cast<std::uint8_t>(block_size));
        out.insert(out.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(block_size));
        block_size = 0;
    }

    std::vector<std::uint8_t>& out;
    std::vector<std::uint32_t> keys;
    std::vector<std::uint16_t> values;
    std::array<std::uint8_t, kMaxBlockSize> block{};
    std::size_t block_size = 0;
    std::uint32_t bit_buffer = 0;
    unsigned bit_count = 0;
    std::uint32_t next_code = kFirstFreeCode;
    unsigned code_width = kInitialCodeWidth;
};

bool PathOccupied(const QString& path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

} // namespace

GifWriter::GifWriter() = default;

GifWriter::~GifWriter() {
    Cancel();
}

bool GifWriter::IsOpen() const {
    return file != nullptr;
}

int GifWriter::FrameCount() const {
    return frame_count;
}

bool GifWriter::Open(const QString& output_path, int frame_width, int frame_height,
                     QString& error) {
    error.clear();
    if (file) {
        error = tr("A GIF recording is already in progress.");
        return false;
    }
    failure.clear();

    if (output_path.isEmpty()) {
        error = tr("No output path was given for the GIF recording.");
        return false;
    }
    if (frame_width < 1 || frame_height < 1 || frame_width > kMaxDimension ||
        frame_height > kMaxDimension) {
        error = tr("The GIF size %1x%2 is outside the supported range of 1 to %3 pixels.")
                    .arg(frame_width)
                    .arg(frame_height)
                    .arg(kMaxDimension);
        return false;
    }
    const QString native_path = QDir::toNativeSeparators(output_path);
    if (PathOccupied(output_path)) {
        error = tr("%1 already exists and will not be overwritten.").arg(native_path);
        return false;
    }

    auto save = std::make_unique<QSaveFile>(output_path);
    if (!save->open(QIODevice::WriteOnly)) {
        error = tr("Could not create %1: %2").arg(native_path, save->errorString());
        return false;
    }
    file = std::move(save);
    path = output_path;
    width = frame_width;
    height = frame_height;
    frame_count = 0;
    indexed.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));

    // Header, logical screen descriptor with a 256-entry global palette, and the NETSCAPE2.0
    // extension that makes the animation loop forever.
    encoded.clear();
    encoded.insert(encoded.end(), std::begin(kSignature), std::end(kSignature));
    PutLe16(encoded, width);
    PutLe16(encoded, height);
    encoded.push_back(0xF7); // Global palette present, 8 bits per channel, 256 entries.
    encoded.push_back(0);    // Background colour index.
    encoded.push_back(0);    // Pixel aspect ratio: unspecified.
    AppendPalette(encoded);
    encoded.insert(encoded.end(), std::begin(kLoopExtension), std::end(kLoopExtension));

    if (const QString message = WriteBytes(encoded); !message.isEmpty()) {
        Cancel();
        error = message;
        return false;
    }
    return true;
}

bool GifWriter::AddFrame(const QImage& image, int delay_cs, QString& error) {
    error.clear();
    if (!file) {
        error = failure.isEmpty() ? tr("No GIF recording is open.") : failure;
        return false;
    }
    if (image.isNull() || image.width() != width || image.height() != height) {
        error = tr("A %1x%2 frame cannot be added to a %3x%4 GIF recording.")
                    .arg(image.width())
                    .arg(image.height())
                    .arg(width)
                    .arg(height);
        return false;
    }
    if (delay_cs < kMinDelayCs || delay_cs > kMaxDelayCs) {
        error = tr("The frame delay of %1 cs is outside the supported range of %2 to %3 cs.")
                    .arg(delay_cs)
                    .arg(kMinDelayCs)
                    .arg(kMaxDelayCs);
        return false;
    }

    QImage rgb = image;
    if (rgb.format() != QImage::Format_RGB32) {
        rgb = rgb.convertToFormat(QImage::Format_RGB32);
        if (rgb.isNull()) {
            error = tr("The frame could not be converted to RGB32.");
            return false;
        }
    }
    QuantizeImage(rgb, indexed);

    encoded.clear();
    // Graphic Control Extension: disposal method 1 (leave in place), no transparency.
    encoded.insert(encoded.end(), {0x21, 0xF9, 0x04, 0x04});
    PutLe16(encoded, delay_cs);
    encoded.insert(encoded.end(), {0x00, 0x00});
    // Image descriptor: full-size frame at the origin, no local palette, not interlaced.
    encoded.push_back(0x2C);
    PutLe16(encoded, 0);
    PutLe16(encoded, 0);
    PutLe16(encoded, width);
    PutLe16(encoded, height);
    encoded.push_back(0x00);
    LzwEncoder(encoded).Encode(indexed.data(), indexed.size());

    if (const QString message = WriteBytes(encoded); !message.isEmpty()) {
        return Fail(message, error);
    }
    ++frame_count;
    return true;
}

bool GifWriter::Finish(QString& error) {
    error.clear();
    if (!file) {
        error = failure.isEmpty() ? tr("No GIF recording is open.") : failure;
        failure.clear();
        return false;
    }
    if (frame_count == 0) {
        Cancel();
        error = tr("The GIF recording contains no frames.");
        return false;
    }

    if (const QString message = WriteBytes({kTrailer}); !message.isEmpty()) {
        Cancel();
        error = message;
        return false;
    }
    if (PathOccupied(path)) {
        error = tr("%1 already exists and will not be overwritten.")
                    .arg(QDir::toNativeSeparators(path));
        Cancel();
        return false;
    }
    if (!file->commit()) {
        error = tr("Could not save %1: %2")
                    .arg(QDir::toNativeSeparators(path), file->errorString());
        Cancel();
        return false;
    }
    Cancel(); // Already committed; this only releases the closed file and resets the state.
    return true;
}

void GifWriter::Cancel() {
    if (file) {
        file->cancelWriting();
        file.reset();
    }
    path.clear();
    failure.clear();
    width = 0;
    height = 0;
    frame_count = 0;
    indexed.clear();
    encoded.clear();
}

QString GifWriter::WriteBytes(const std::vector<std::uint8_t>& data) {
    const auto size = static_cast<qint64>(data.size());
    if (file->write(reinterpret_cast<const char*>(data.data()), size) != size || !file->flush()) {
        return tr("Could not write %1: %2")
            .arg(QDir::toNativeSeparators(path), file->errorString());
    }
    return {};
}

bool GifWriter::Fail(const QString& message, QString& error) {
    file->cancelWriting();
    file.reset();
    indexed.clear();
    encoded.clear();
    failure = message;
    error = message;
    return false;
}
