// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>

#include "yuzu/macro_file.h"

namespace EdenMacro {

namespace {

struct Text {
    Q_DECLARE_TR_FUNCTIONS(EdenMacro)
};

constexpr double StickLimit = 1.0;
constexpr double TriggerLimit = 1.0;
constexpr double MotionLimit = 100.0;

// All bounded numeric fields use this: false for NaN, and the bounds exclude infinities.
bool InRange(double value, double low, double high) {
    return value >= low && value <= high;
}

bool Fail(QString& error, QString message) {
    error = std::move(message);
    return false;
}

QString HexDigits() {
    return QStringLiteral("0123456789abcdefABCDEF");
}

// Number of Unicode code points in text, or -1 if it is malformed UTF-16 (unpaired surrogate) or
// contains a control character.
int CodePoints(const QString& text) {
    int count = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar unit = text.at(i);
        char32_t code_point = unit.unicode();
        if (unit.isHighSurrogate()) {
            if (i + 1 >= text.size() || !text.at(i + 1).isLowSurrogate()) {
                return -1;
            }
            code_point = QChar::surrogateToUcs4(unit, text.at(i + 1));
            ++i;
        } else if (unit.isLowSurrogate()) {
            return -1;
        }
        if (QChar::category(code_point) == QChar::Other_Control) {
            return -1;
        }
        ++count;
    }
    return count;
}

// Returns an empty string if text is acceptable free text of at most max_chars code points.
QString TextError(const QString& text, int max_chars) {
    const int count = CodePoints(text);
    if (count < 0) {
        return Text::tr("contains control characters or invalid UTF-16");
    }
    if (count > max_chars) {
        return Text::tr("is longer than %1 characters").arg(max_chars);
    }
    return {};
}

// Returns an empty string if name is a valid macro/marker name.
QString NameError(const QString& name) {
    if (name.isEmpty()) {
        return Text::tr("must not be empty");
    }
    if (name != name.trimmed()) {
        return Text::tr("must not start or end with whitespace");
    }
    return TextError(name, MaxNameChars);
}

QString SampleError(const Sample& sample) {
    if ((sample.buttons & ~ButtonMask) != 0) {
        return Text::tr("buttons has bits outside the %1 button mask").arg(ButtonCount);
    }
    for (const float stick : sample.sticks) {
        if (!InRange(static_cast<double>(stick), -StickLimit, StickLimit)) {
            return Text::tr("sticks must be finite numbers in [-1, 1]");
        }
    }
    for (const float trigger : sample.triggers) {
        if (!InRange(static_cast<double>(trigger), 0.0, TriggerLimit)) {
            return Text::tr("triggers must be finite numbers in [0, 1]");
        }
    }
    for (const Motion& motion : sample.motion) {
        if (motion.delta_us < MinMotionDeltaUs || motion.delta_us > MaxMotionDeltaUs) {
            return Text::tr("motion delta_us must be in [%1, %2]")
                .arg(MinMotionDeltaUs)
                .arg(MaxMotionDeltaUs);
        }
        for (const auto* axes : {&motion.gyro, &motion.accel}) {
            for (const float value : *axes) {
                if (!InRange(static_cast<double>(value), -MotionLimit, MotionLimit)) {
                    return Text::tr("motion gyro/accel must be finite numbers in [-100, 100]");
                }
            }
        }
    }
    return {};
}

// ---------------------------------------------------------------------------------------------
// JSON writing
// ---------------------------------------------------------------------------------------------

// The shortest of 6 or 9 significant digits that reads back as exactly the same float (9 digits
// always do), so files stay compact and values round-trip bit-exactly.
QJsonValue FloatValue(float value) {
    const double exact = static_cast<double>(value);
    const double short_form = QString::number(exact, 'g', 6).toDouble();
    if (static_cast<float>(short_form) == value) {
        return short_form;
    }
    return QString::number(exact, 'g', 9).toDouble();
}

template <std::size_t N>
QJsonArray FloatArray(const std::array<float, N>& values) {
    QJsonArray array;
    for (const float value : values) {
        array.append(FloatValue(value));
    }
    return array;
}

QJsonObject EventToJson(const Event& event) {
    QJsonArray motion;
    for (const Motion& m : event.sample.motion) {
        motion.append(QJsonObject{
            {QStringLiteral("delta_us"), m.delta_us},
            {QStringLiteral("gyro"), FloatArray(m.gyro)},
            {QStringLiteral("accel"), FloatArray(m.accel)},
        });
    }
    return {
        {QStringLiteral("frame"), static_cast<qint64>(event.frame)},
        {QStringLiteral("buttons"), static_cast<qint64>(event.sample.buttons)},
        {QStringLiteral("sticks"), FloatArray(event.sample.sticks)},
        {QStringLiteral("triggers"), FloatArray(event.sample.triggers)},
        {QStringLiteral("motion"), motion},
    };
}

// ---------------------------------------------------------------------------------------------
// JSON reading. Helpers set `detail` (a reason without location) on failure.
// ---------------------------------------------------------------------------------------------

bool KeysExactly(const QJsonObject& object, const QStringList& keys, QString& detail) {
    for (const QString& key : keys) {
        if (!object.contains(key)) {
            detail = Text::tr("missing key \"%1\"").arg(key);
            return false;
        }
    }
    if (object.size() != keys.size()) {
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!keys.contains(it.key())) {
                detail = Text::tr("unknown key \"%1\"").arg(it.key());
                return false;
            }
        }
    }
    return true;
}

bool ReadObject(const QJsonValue& value, const QStringList& keys, QJsonObject& out,
                QString& detail) {
    if (!value.isObject()) {
        detail = Text::tr("must be an object");
        return false;
    }
    out = value.toObject();
    return KeysExactly(out, keys, detail);
}

bool ReadString(const QJsonValue& value, QString& out, QString& detail) {
    if (!value.isString()) {
        detail = Text::tr("must be a string");
        return false;
    }
    out = value.toString();
    return true;
}

bool ReadInteger(const QJsonValue& value, qint64 low, qint64 high, qint64& out, QString& detail) {
    if (value.type() != QJsonValue::Double) {
        detail = Text::tr("must be a number");
        return false;
    }
    const double number = value.toDouble();
    if (!InRange(number, static_cast<double>(low), static_cast<double>(high)) ||
        number != static_cast<double>(static_cast<qint64>(number))) {
        detail = Text::tr("must be an integer in [%1, %2]").arg(low).arg(high);
        return false;
    }
    out = static_cast<qint64>(number);
    return true;
}

template <std::size_t N>
bool ReadFloats(const QJsonValue& value, double low, double high, std::array<float, N>& out,
                QString& detail) {
    if (!value.isArray() || value.toArray().size() != static_cast<qsizetype>(N)) {
        detail = Text::tr("must be an array of %1 numbers").arg(N);
        return false;
    }
    const QJsonArray array = value.toArray();
    for (qsizetype i = 0; i < array.size(); ++i) {
        const QJsonValue item = array.at(i);
        if (item.type() != QJsonValue::Double || !InRange(item.toDouble(), low, high)) {
            detail = Text::tr("must contain only numbers in [%1, %2]").arg(low).arg(high);
            return false;
        }
        out[static_cast<std::size_t>(i)] = static_cast<float>(item.toDouble());
    }
    return true;
}

bool ReadMotion(const QJsonValue& value, Motion& out, QString& detail) {
    QJsonObject object;
    if (!ReadObject(value, {QStringLiteral("delta_us"), QStringLiteral("gyro"),
                            QStringLiteral("accel")},
                    object, detail)) {
        return false;
    }
    qint64 delta = 0;
    if (!ReadInteger(object.value(QStringLiteral("delta_us")), MinMotionDeltaUs, MaxMotionDeltaUs,
                     delta, detail)) {
        detail = QStringLiteral("delta_us ") + detail;
        return false;
    }
    out.delta_us = delta;
    if (!ReadFloats(object.value(QStringLiteral("gyro")), -MotionLimit, MotionLimit, out.gyro,
                    detail)) {
        detail = QStringLiteral("gyro ") + detail;
        return false;
    }
    if (!ReadFloats(object.value(QStringLiteral("accel")), -MotionLimit, MotionLimit, out.accel,
                    detail)) {
        detail = QStringLiteral("accel ") + detail;
        return false;
    }
    return true;
}

bool ReadEvent(const QJsonValue& value, Event& out, QString& detail) {
    QJsonObject object;
    if (!ReadObject(value,
                    {QStringLiteral("frame"), QStringLiteral("buttons"), QStringLiteral("sticks"),
                     QStringLiteral("triggers"), QStringLiteral("motion")},
                    object, detail)) {
        return false;
    }
    qint64 number = 0;
    if (!ReadInteger(object.value(QStringLiteral("frame")), 0, static_cast<qint64>(MaxFrames),
                     number, detail)) {
        detail = QStringLiteral("frame ") + detail;
        return false;
    }
    out.frame = static_cast<quint64>(number);
    if (!ReadInteger(object.value(QStringLiteral("buttons")), 0, ButtonMask, number, detail)) {
        detail = QStringLiteral("buttons ") + detail;
        return false;
    }
    out.sample.buttons = static_cast<quint32>(number);
    if (!ReadFloats(object.value(QStringLiteral("sticks")), -StickLimit, StickLimit,
                    out.sample.sticks, detail)) {
        detail = QStringLiteral("sticks ") + detail;
        return false;
    }
    if (!ReadFloats(object.value(QStringLiteral("triggers")), 0.0, TriggerLimit,
                    out.sample.triggers, detail)) {
        detail = QStringLiteral("triggers ") + detail;
        return false;
    }
    const QJsonValue motion = object.value(QStringLiteral("motion"));
    if (!motion.isArray() || motion.toArray().size() != 2) {
        detail = Text::tr("motion must be an array of 2 objects");
        return false;
    }
    const QJsonArray motion_array = motion.toArray();
    for (qsizetype i = 0; i < 2; ++i) {
        if (!ReadMotion(motion_array.at(i), out.sample.motion[static_cast<std::size_t>(i)],
                        detail)) {
            detail = QStringLiteral("motion[%1] ").arg(i) + detail;
            return false;
        }
    }
    return true;
}

} // namespace

QString FormatName() {
    return QStringLiteral("eden-input-macro");
}

QStringList ButtonNames() {
    return {QStringLiteral("a"),        QStringLiteral("b"),        QStringLiteral("x"),
            QStringLiteral("y"),        QStringLiteral("lstick"),   QStringLiteral("rstick"),
            QStringLiteral("l"),        QStringLiteral("r"),        QStringLiteral("zl"),
            QStringLiteral("zr"),       QStringLiteral("plus"),     QStringLiteral("minus"),
            QStringLiteral("left"),     QStringLiteral("up"),       QStringLiteral("right"),
            QStringLiteral("down"),     QStringLiteral("sl_left"),  QStringLiteral("sr_left"),
            QStringLiteral("home"),     QStringLiteral("capture"),  QStringLiteral("sl_right"),
            QStringLiteral("sr_right")};
}

bool Validate(const Document& document, QString& error) {
    if (const QString why = NameError(document.name); !why.isEmpty()) {
        return Fail(error, Text::tr("name %1").arg(why));
    }
    if (!document.game_id.isEmpty()) {
        const QString digits = HexDigits();
        bool ok = document.game_id.size() == GameIdChars;
        for (const QChar c : document.game_id) {
            ok = ok && digits.contains(c);
        }
        if (!ok) {
            return Fail(error, Text::tr("game_id must be empty or %1 hexadecimal digits")
                                   .arg(GameIdChars));
        }
    }
    if (document.player < 0 || document.player > MaxPlayer) {
        return Fail(error, Text::tr("player must be in [0, %1]").arg(MaxPlayer));
    }
    if (document.duration_frames < 1 || document.duration_frames > MaxFrames) {
        return Fail(error,
                    Text::tr("duration_frames must be in [1, %1]").arg(MaxFrames));
    }
    if (const QString why = TextError(document.keyboard_shortcut, MaxShortcutChars);
        !why.isEmpty()) {
        return Fail(error, Text::tr("keyboard shortcut %1").arg(why));
    }
    if (const QString why = TextError(document.gamepad_shortcut, MaxShortcutChars);
        !why.isEmpty()) {
        return Fail(error, Text::tr("gamepad shortcut %1").arg(why));
    }
    if (document.parent_name.isEmpty() != document.parent_marker.isEmpty()) {
        return Fail(error, Text::tr("parent name and parent marker must be set together"));
    }
    if (!document.parent_name.isEmpty()) {
        if (const QString why = NameError(document.parent_name); !why.isEmpty()) {
            return Fail(error, Text::tr("parent name %1").arg(why));
        }
        if (const QString why = NameError(document.parent_marker); !why.isEmpty()) {
            return Fail(error, Text::tr("parent marker %1").arg(why));
        }
        if (document.parent_name == document.name) {
            return Fail(error, Text::tr("a macro cannot be its own parent"));
        }
    }

    if (document.events.empty()) {
        return Fail(error, Text::tr("the macro has no recorded frames"));
    }
    if (document.events.size() > static_cast<std::size_t>(MaxEvents)) {
        return Fail(error, Text::tr("more than %1 events").arg(MaxEvents));
    }
    if (document.events.front().frame != 0) {
        return Fail(error, Text::tr("the first event must be at frame 0"));
    }
    for (std::size_t i = 0; i < document.events.size(); ++i) {
        const Event& event = document.events[i];
        if (i > 0 && event.frame <= document.events[i - 1].frame) {
            return Fail(error, Text::tr("events[%1]: frames must be strictly ascending").arg(i));
        }
        if (event.frame >= document.duration_frames) {
            return Fail(error, Text::tr("events[%1]: frame is not before duration_frames").arg(i));
        }
        if (const QString why = SampleError(event.sample); !why.isEmpty()) {
            return Fail(error, QStringLiteral("events[%1]: ").arg(i) + why);
        }
    }

    if (document.markers.size() > static_cast<std::size_t>(MaxMarkers)) {
        return Fail(error, Text::tr("more than %1 markers").arg(MaxMarkers));
    }
    QSet<QString> names;
    for (std::size_t i = 0; i < document.markers.size(); ++i) {
        const Marker& marker = document.markers[i];
        if (const QString why = NameError(marker.name); !why.isEmpty()) {
            return Fail(error, Text::tr("markers[%1]: name %2").arg(i).arg(why));
        }
        if (names.contains(marker.name)) {
            return Fail(error, Text::tr("markers[%1]: duplicate marker name").arg(i));
        }
        names.insert(marker.name);
        if (marker.frame > document.duration_frames) {
            return Fail(error, Text::tr("markers[%1]: frame is after duration_frames").arg(i));
        }
    }
    error.clear();
    return true;
}

QJsonObject ToJson(const Document& document) {
    QJsonArray events;
    for (const Event& event : document.events) {
        events.append(EventToJson(event));
    }
    QJsonArray markers;
    for (const Marker& marker : document.markers) {
        markers.append(QJsonObject{{QStringLiteral("name"), marker.name},
                                   {QStringLiteral("frame"), static_cast<qint64>(marker.frame)}});
    }
    return {
        {QStringLiteral("format"), FormatName()},
        {QStringLiteral("version"), FormatVersion},
        {QStringLiteral("name"), document.name},
        {QStringLiteral("game_id"), document.game_id},
        {QStringLiteral("player"), document.player},
        {QStringLiteral("duration_frames"), static_cast<qint64>(document.duration_frames)},
        {QStringLiteral("bindings"),
         QJsonObject{{QStringLiteral("keyboard"), document.keyboard_shortcut},
                     {QStringLiteral("gamepad"), document.gamepad_shortcut}}},
        {QStringLiteral("parent"),
         QJsonObject{{QStringLiteral("name"), document.parent_name},
                     {QStringLiteral("marker"), document.parent_marker}}},
        {QStringLiteral("markers"), markers},
        {QStringLiteral("events"), events},
    };
}

bool FromJson(const QJsonObject& object, Document& out, QString& error) {
    QString detail;
    const QJsonValue format = object.value(QStringLiteral("format"));
    if (!format.isString() || format.toString() != FormatName()) {
        return Fail(error, Text::tr("not an %1 file").arg(FormatName()));
    }
    qint64 number = 0;
    const QJsonValue version = object.value(QStringLiteral("version"));
    if (version.type() != QJsonValue::Double || version.toDouble() != FormatVersion) {
        return Fail(error, Text::tr("unsupported macro file version"));
    }
    if (!KeysExactly(object,
                     {QStringLiteral("format"), QStringLiteral("version"), QStringLiteral("name"),
                      QStringLiteral("game_id"), QStringLiteral("player"),
                      QStringLiteral("duration_frames"), QStringLiteral("bindings"),
                      QStringLiteral("parent"), QStringLiteral("markers"),
                      QStringLiteral("events")},
                     detail)) {
        return Fail(error, detail);
    }

    Document parsed;
    if (!ReadString(object.value(QStringLiteral("name")), parsed.name, detail)) {
        return Fail(error, QStringLiteral("name ") + detail);
    }
    if (!ReadString(object.value(QStringLiteral("game_id")), parsed.game_id, detail)) {
        return Fail(error, QStringLiteral("game_id ") + detail);
    }
    if (!ReadInteger(object.value(QStringLiteral("player")), 0, MaxPlayer, number, detail)) {
        return Fail(error, QStringLiteral("player ") + detail);
    }
    parsed.player = static_cast<int>(number);
    if (!ReadInteger(object.value(QStringLiteral("duration_frames")), 1,
                     static_cast<qint64>(MaxFrames), number, detail)) {
        return Fail(error, QStringLiteral("duration_frames ") + detail);
    }
    parsed.duration_frames = static_cast<quint64>(number);

    QJsonObject nested;
    if (!ReadObject(object.value(QStringLiteral("bindings")),
                    {QStringLiteral("keyboard"), QStringLiteral("gamepad")}, nested, detail)) {
        return Fail(error, QStringLiteral("bindings ") + detail);
    }
    if (!ReadString(nested.value(QStringLiteral("keyboard")), parsed.keyboard_shortcut, detail) ||
        !ReadString(nested.value(QStringLiteral("gamepad")), parsed.gamepad_shortcut, detail)) {
        return Fail(error, QStringLiteral("bindings ") + detail);
    }
    if (!ReadObject(object.value(QStringLiteral("parent")),
                    {QStringLiteral("name"), QStringLiteral("marker")}, nested, detail)) {
        return Fail(error, QStringLiteral("parent ") + detail);
    }
    if (!ReadString(nested.value(QStringLiteral("name")), parsed.parent_name, detail) ||
        !ReadString(nested.value(QStringLiteral("marker")), parsed.parent_marker, detail)) {
        return Fail(error, QStringLiteral("parent ") + detail);
    }

    const QJsonValue markers = object.value(QStringLiteral("markers"));
    if (!markers.isArray()) {
        return Fail(error, Text::tr("markers must be an array"));
    }
    const QJsonArray marker_array = markers.toArray();
    if (marker_array.size() > MaxMarkers) {
        return Fail(error, Text::tr("more than %1 markers").arg(MaxMarkers));
    }
    parsed.markers.reserve(static_cast<std::size_t>(marker_array.size()));
    for (qsizetype i = 0; i < marker_array.size(); ++i) {
        QJsonObject marker;
        Marker item;
        if (!ReadObject(marker_array.at(i), {QStringLiteral("name"), QStringLiteral("frame")},
                        marker, detail) ||
            !ReadString(marker.value(QStringLiteral("name")), item.name, detail)) {
            return Fail(error, QStringLiteral("markers[%1]: ").arg(i) + detail);
        }
        if (!ReadInteger(marker.value(QStringLiteral("frame")), 0, static_cast<qint64>(MaxFrames),
                         number, detail)) {
            return Fail(error, QStringLiteral("markers[%1]: frame ").arg(i) + detail);
        }
        item.frame = static_cast<quint64>(number);
        parsed.markers.push_back(std::move(item));
    }

    const QJsonValue events = object.value(QStringLiteral("events"));
    if (!events.isArray()) {
        return Fail(error, Text::tr("events must be an array"));
    }
    const QJsonArray event_array = events.toArray();
    if (event_array.size() > MaxEvents) {
        return Fail(error, Text::tr("more than %1 events").arg(MaxEvents));
    }
    parsed.events.resize(static_cast<std::size_t>(event_array.size()));
    for (qsizetype i = 0; i < event_array.size(); ++i) {
        if (!ReadEvent(event_array.at(i), parsed.events[static_cast<std::size_t>(i)], detail)) {
            return Fail(error, QStringLiteral("events[%1]: ").arg(i) + detail);
        }
    }

    if (!Validate(parsed, error)) {
        return false;
    }
    out = std::move(parsed);
    error.clear();
    return true;
}

bool Load(const QString& path, Document& out, QString& error) {
    const QFileInfo info(path);
    if (path.isEmpty() || !info.exists()) {
        return Fail(error, Text::tr("The macro file does not exist."));
    }
    if (!info.isFile()) {
        return Fail(error, Text::tr("The macro path is not a regular file."));
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return Fail(error, Text::tr("Cannot open the macro file: %1").arg(file.errorString()));
    }
    if (file.size() > MaxFileBytes) {
        return Fail(error, Text::tr("The macro file is larger than %1 MiB.")
                               .arg(MaxFileBytes / (1024 * 1024)));
    }
    // The file may grow after the size check: never read more than the limit plus one byte.
    QByteArray data = file.read(MaxFileBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        return Fail(error, Text::tr("Cannot read the macro file: %1").arg(file.errorString()));
    }
    file.close();
    if (data.size() > MaxFileBytes) {
        return Fail(error, Text::tr("The macro file is larger than %1 MiB.")
                               .arg(MaxFileBytes / (1024 * 1024)));
    }

    QJsonParseError parse_error;
    const QJsonDocument json = QJsonDocument::fromJson(data, &parse_error);
    data.clear();
    data.squeeze();
    if (parse_error.error != QJsonParseError::NoError) {
        return Fail(error, Text::tr("The macro file is not valid JSON (byte %1: %2).")
                               .arg(parse_error.offset)
                               .arg(parse_error.errorString()));
    }
    if (!json.isObject()) {
        return Fail(error, Text::tr("The macro file must contain a JSON object."));
    }
    return FromJson(json.object(), out, error);
}

bool Save(const QString& path, const Document& document, QString& error, bool overwrite) {
    if (!Validate(document, error)) {
        return false;
    }
    if (path.isEmpty()) {
        return Fail(error, Text::tr("No macro file path was given."));
    }
    // isSymLink() does not follow the link, so it also catches dangling links.
    const auto check_target = [&]() -> QString {
        const QFileInfo info(path);
        if (info.isSymLink()) {
            return Text::tr("The destination is a symbolic link.");
        }
        if (info.exists()) {
            if (!info.isFile()) {
                return Text::tr("The destination is not a regular file.");
            }
            if (!overwrite) {
                return Text::tr("The destination already exists.");
            }
        }
        return {};
    };
    if (const QString why = check_target(); !why.isEmpty()) {
        return Fail(error, why);
    }

    QByteArray bytes = QJsonDocument(ToJson(document)).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    if (bytes.size() > MaxFileBytes) {
        return Fail(error, Text::tr("The macro is larger than %1 MiB once saved.")
                               .arg(MaxFileBytes / (1024 * 1024)));
    }

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return Fail(error, Text::tr("Cannot create the macro file: %1").arg(file.errorString()));
    }
    if (file.write(bytes) != bytes.size()) {
        const QString reason = file.errorString();
        file.cancelWriting();
        return Fail(error, Text::tr("Cannot write the macro file: %1").arg(reason));
    }
    // The commit renames over the destination, so look again just before publishing.
    if (const QString why = check_target(); !why.isEmpty()) {
        file.cancelWriting();
        return Fail(error, why);
    }
    if (!file.commit()) {
        return Fail(error, Text::tr("Cannot finish the macro file: %1").arg(file.errorString()));
    }
    error.clear();
    return true;
}

bool MakePrefix(const Document& source, const QString& marker_name, const QString& new_name,
                Document& out, QString& error) {
    QString detail;
    if (!Validate(source, detail)) {
        return Fail(error, Text::tr("The source macro is invalid: %1").arg(detail));
    }
    if (const QString why = NameError(new_name); !why.isEmpty()) {
        return Fail(error, Text::tr("The new macro name %1").arg(why));
    }
    if (new_name == source.name) {
        return Fail(error, Text::tr("The new macro needs a name different from its parent."));
    }
    const auto marker = std::ranges::find(source.markers, marker_name, &Marker::name);
    if (marker == source.markers.end()) {
        return Fail(error, Text::tr("The source macro has no marker named \"%1\".")
                               .arg(marker_name));
    }

    const quint64 cut = marker->frame;
    Document prefix;
    prefix.name = new_name;
    prefix.game_id = source.game_id;
    prefix.player = source.player;
    prefix.duration_frames = cut;
    prefix.parent_name = source.name;
    prefix.parent_marker = marker->name;
    const auto end = std::ranges::lower_bound(source.events, cut, {}, &Event::frame);
    prefix.events.assign(source.events.begin(), end);
    std::ranges::copy_if(source.markers, std::back_inserter(prefix.markers),
                         [cut](const Marker& m) { return m.frame <= cut; });

    out = std::move(prefix);
    error.clear();
    return true;
}

const Sample& SampleAt(const Document& document, quint64 frame) {
    static const Sample neutral{};
    const auto after = std::ranges::upper_bound(document.events, frame, {}, &Event::frame);
    if (after == document.events.begin()) {
        return neutral;
    }
    return std::prev(after)->sample;
}

} // namespace EdenMacro
