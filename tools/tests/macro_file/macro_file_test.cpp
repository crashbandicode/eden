// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Tests for the macro file codec (src/yuzu/macro_file.*). The expectations come from the written
// format specification (hand-written JSON, explicit numbers and invariants), not from running
// the code under test over itself. The program also writes real macro files and a manifest into
// the directory given as argv[1]; validate_macros.py re-reads them with an independent JSON
// parser and checks the on-disk format.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <random>
#include <utility>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#include "yuzu/macro_file.h"

using namespace EdenMacro;

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
    std::fprintf(stderr, "FAIL %s:%d: %s %s\n", file, line, expression, qPrintable(detail));
}

#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define CHECK_MSG(expression, detail) \
    Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__, (detail))
#define K(text) QStringLiteral(text)

// ---------------------------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------------------------

// Hand-written file in the documented layout (does not go through ToJson).
const char* const GoldenText = R"json({
  "format": "eden-input-macro", "version": 1,
  "name": "Golden run", "game_id": "0100ABCDEF012000", "player": 3, "duration_frames": 120,
  "bindings": {"keyboard": "Ctrl+F5", "gamepad": "L+R"},
  "parent": {"name": "Base", "marker": "m"},
  "markers": [{"name": "start", "frame": 0}, {"name": "Start", "frame": 60},
              {"name": "end", "frame": 120}],
  "events": [
    {"frame": 0, "buttons": 0, "sticks": [0, 0, 0, 0], "triggers": [0, 0],
     "motion": [{"delta_us": 10000, "gyro": [0, 0, 0], "accel": [0, 0, -1]},
                {"delta_us": 10000, "gyro": [0, 0, 0], "accel": [0, 0, -1]}]},
    {"frame": 30, "buttons": 2097153, "sticks": [0.5, -1, 1, -0.25], "triggers": [0.75, 1],
     "motion": [{"delta_us": 5000, "gyro": [1.5, -2.5, 100], "accel": [0, -100, 0.125]},
                {"delta_us": 1000000, "gyro": [-100, 0, 0.5], "accel": [0.25, 0.5, -1]}]}
  ]
})json";

QJsonObject Golden() {
    return QJsonDocument::fromJson(QByteArray(GoldenText)).object();
}

Document GoldenDocument() {
    Document doc;
    doc.name = K("Golden run");
    doc.game_id = K("0100ABCDEF012000");
    doc.player = 3;
    doc.duration_frames = 120;
    doc.keyboard_shortcut = K("Ctrl+F5");
    doc.gamepad_shortcut = K("L+R");
    doc.parent_name = K("Base");
    doc.parent_marker = K("m");
    doc.markers = {{K("start"), 0}, {K("Start"), 60}, {K("end"), 120}};
    Event first;
    first.frame = 0;
    Event second;
    second.frame = 30;
    second.sample.buttons = 0x200001;
    second.sample.sticks = {0.5f, -1.0f, 1.0f, -0.25f};
    second.sample.triggers = {0.75f, 1.0f};
    second.sample.motion[0] = {5000, {1.5f, -2.5f, 100.0f}, {0.0f, -100.0f, 0.125f}};
    second.sample.motion[1] = {1000000, {-100.0f, 0.0f, 0.5f}, {0.25f, 0.5f, -1.0f}};
    doc.events = {first, second};
    return doc;
}

bool SameDocument(const Document& a, const Document& b) {
    if (a.name != b.name || a.game_id != b.game_id || a.player != b.player ||
        a.duration_frames != b.duration_frames || a.keyboard_shortcut != b.keyboard_shortcut ||
        a.gamepad_shortcut != b.gamepad_shortcut || a.parent_name != b.parent_name ||
        a.parent_marker != b.parent_marker || a.events.size() != b.events.size() ||
        a.markers.size() != b.markers.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.events.size(); ++i) {
        if (a.events[i].frame != b.events[i].frame || !(a.events[i].sample == b.events[i].sample)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < a.markers.size(); ++i) {
        if (a.markers[i].name != b.markers[i].name || a.markers[i].frame != b.markers[i].frame) {
            return false;
        }
    }
    return true;
}

Document Sentinel() {
    Document doc;
    doc.name = K("sentinel");
    doc.duration_frames = 77;
    return doc;
}

bool IsSentinel(const Document& doc) {
    return doc.name == K("sentinel") && doc.duration_frames == 77 && doc.events.empty() &&
           doc.markers.empty();
}

QJsonArray Numbers(std::initializer_list<double> values) {
    QJsonArray array;
    for (const double value : values) {
        array.append(value);
    }
    return array;
}

using Edit = std::function<void(QJsonObject&)>;

Edit EditEvent(int index, std::function<void(QJsonObject&)> change) {
    return [=](QJsonObject& root) {
        QJsonArray events = root[K("events")].toArray();
        QJsonObject event = events[index].toObject();
        change(event);
        events[index] = event;
        root[K("events")] = events;
    };
}

Edit EditMotion(int event_index, int motion_index, std::function<void(QJsonObject&)> change) {
    return EditEvent(event_index, [=](QJsonObject& event) {
        QJsonArray motion = event[K("motion")].toArray();
        QJsonObject item = motion[motion_index].toObject();
        change(item);
        motion[motion_index] = item;
        event[K("motion")] = motion;
    });
}

/// FromJson on the golden object with `edit` applied must fail with a message and leave `out`.
void ExpectRejected(const Edit& edit, const char* what) {
    QJsonObject object = Golden();
    edit(object);
    Document out = Sentinel();
    QString error;
    const bool ok = FromJson(object, out, error);
    CHECK_MSG(!ok, QString::fromUtf8(what));
    CHECK_MSG(!ok && !error.isEmpty(), QString::fromUtf8(what));
    CHECK_MSG(IsSentinel(out), QString::fromUtf8(what));
}

void ExpectAccepted(const Edit& edit, const char* what) {
    QJsonObject object = Golden();
    edit(object);
    Document out;
    QString error;
    CHECK_MSG(FromJson(object, out, error), QString::fromUtf8(what) + K(": ") + error);
}

QString Repeat(const QString& text, int count) {
    return text.repeated(count);
}

QStringList DirListing(const QString& dir) {
    return QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                               QDir::Name);
}

QByteArray ReadAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool WriteAll(const QString& path, const QByteArray& data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.flush();
}

// A larger document with varied, deterministic data.
Document MakeSourceDocument() {
    Document doc;
    doc.name = K("Source");
    doc.game_id = K("0100000000010000");
    doc.player = 2;
    doc.duration_frames = 200;
    doc.keyboard_shortcut = K("F5");
    doc.gamepad_shortcut = K("ZL+ZR");
    const quint64 frames[] = {0, 10, 25, 100};
    for (std::size_t i = 0; i < 4; ++i) {
        Event event;
        event.frame = frames[i];
        event.sample.buttons = 1u << i;
        event.sample.sticks[0] = 0.1f * static_cast<float>(i);
        event.sample.motion[1].gyro[2] = static_cast<float>(i) - 1.5f;
        doc.events.push_back(event);
    }
    doc.markers = {{K("m0"), 0}, {K("ten"), 10}, {K("eleven"), 11}, {K("boss"), 25},
                   {K("Boss"), 26}, {K("late"), 100}, {K("end"), 200}};
    return doc;
}

// ---------------------------------------------------------------------------------------------
// Decoding the specification's own example, and the layout the encoder produces
// ---------------------------------------------------------------------------------------------

void TestGolden() {
    Document decoded;
    QString error;
    CHECK_MSG(FromJson(Golden(), decoded, error), error);
    CHECK(SameDocument(decoded, GoldenDocument()));
    CHECK(Validate(GoldenDocument(), error));
    CHECK(error.isEmpty());

    // The encoder must produce exactly the specified structure (numbers compare by value).
    const QJsonObject encoded = ToJson(GoldenDocument());
    CHECK(encoded == Golden());
    CHECK(encoded.keys().size() == 10);
}

void TestFloatRoundTrip() {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::uniform_real_distribution<float> wide(-100.0f, 100.0f);
    const float specials[] = {0.1f, 1.0f / 3.0f, 1.4e-45f, 1.17549435e-38f, 99.99999f,
                              -100.0f, 100.0f, 0.99999994f, 1.0e-7f};
    Document doc = GoldenDocument();
    doc.duration_frames = 5000;
    doc.events.clear();
    for (quint64 i = 0; i < 1500; ++i) {
        Event event;
        event.frame = i * 3;
        event.sample.buttons = static_cast<quint32>(rng()) & ButtonMask;
        for (float& stick : event.sample.sticks) {
            stick = unit(rng);
        }
        for (float& trigger : event.sample.triggers) {
            trigger = std::abs(unit(rng));
        }
        for (Motion& motion : event.sample.motion) {
            motion.delta_us = 1 + static_cast<qint64>(rng() % 1000000);
            for (float& v : motion.gyro) {
                v = wide(rng);
            }
            for (float& v : motion.accel) {
                v = wide(rng);
            }
        }
        if (i < 9) {
            event.sample.motion[0].gyro[0] = specials[i];
            event.sample.motion[1].accel[2] = -specials[i];
            event.sample.sticks[1] = std::fabs(specials[i]) <= 1.0f ? specials[i] : 0.5f;
        }
        doc.events.push_back(event);
    }
    QString error;
    CHECK_MSG(Validate(doc, error), error);

    Document via_json;
    CHECK_MSG(FromJson(ToJson(doc), via_json, error), error);
    CHECK(SameDocument(doc, via_json));

    // And through the actual bytes (text number formatting included).
    QTemporaryDir dir;
    const QString path = dir.filePath(K("floats.json"));
    CHECK_MSG(Save(path, doc, error), error);
    Document loaded;
    CHECK_MSG(Load(path, loaded, error), error);
    CHECK(SameDocument(doc, loaded));
}

// ---------------------------------------------------------------------------------------------
// Rejection of malformed content
// ---------------------------------------------------------------------------------------------

void TestRootRules() {
    ExpectAccepted([](QJsonObject&) {}, "unmodified golden object");
    ExpectRejected([](QJsonObject& o) { o[K("format")] = K("eden-input-macro2"); }, "other format");
    ExpectRejected([](QJsonObject& o) { o[K("format")] = 5; }, "format type");
    ExpectRejected([](QJsonObject& o) { o[K("version")] = 2; }, "version 2");
    ExpectRejected([](QJsonObject& o) { o[K("version")] = 0; }, "version 0");
    ExpectRejected([](QJsonObject& o) { o[K("version")] = 1.5; }, "version 1.5");
    ExpectRejected([](QJsonObject& o) { o[K("version")] = K("1"); }, "version as string");
    ExpectRejected([](QJsonObject& o) { o[K("version")] = true; }, "version as bool");
    ExpectRejected([](QJsonObject& o) { o.remove(K("version")); }, "missing version");
    ExpectRejected([](QJsonObject& o) { o.remove(K("format")); }, "missing format");
    for (const QString& key : Golden().keys()) {
        ExpectRejected([key](QJsonObject& o) { o.remove(key); }, "missing root key");
    }
    ExpectRejected([](QJsonObject& o) { o[K("extra")] = 1; }, "unknown root key");
    ExpectRejected([](QJsonObject& o) {
        QJsonObject b = o[K("bindings")].toObject();
        b[K("mouse")] = K("x");
        o[K("bindings")] = b;
    }, "unknown bindings key");
    ExpectRejected([](QJsonObject& o) {
        QJsonObject b = o[K("bindings")].toObject();
        b.remove(K("gamepad"));
        o[K("bindings")] = b;
    }, "missing bindings key");
    ExpectRejected([](QJsonObject& o) { o[K("parent")] = QJsonObject{{K("name"), K("Base")}}; },
                   "parent without marker key");
    ExpectRejected([](QJsonObject& o) { o[K("bindings")] = K("none"); }, "bindings type");
}

void TestScalarFieldRules() {
    // name
    ExpectRejected([](QJsonObject& o) { o[K("name")] = 5; }, "name number");
    ExpectRejected([](QJsonObject& o) { o[K("name")] = QJsonValue::Null; }, "name null");
    ExpectRejected([](QJsonObject& o) { o[K("name")] = K(""); }, "empty name");
    ExpectRejected([](QJsonObject& o) { o[K("name")] = K(" lead"); }, "leading space");
    ExpectRejected([](QJsonObject& o) { o[K("name")] = K("trail "); }, "trailing space");
    ExpectRejected([](QJsonObject& o) { o[K("name")] = K("\U000000A0nbsp"); }, "leading NBSP");
    ExpectRejected([](QJsonObject& o) { o[K("name")] = K("tab\there"); }, "control char");
    ExpectRejected([](QJsonObject& o) { o[K("name")] = Repeat(K("n"), 129); }, "129 chars");
    ExpectAccepted([](QJsonObject& o) { o[K("name")] = Repeat(K("n"), 128); }, "128 chars");
    ExpectAccepted([](QJsonObject& o) { o[K("name")] = K("in ner  space"); }, "inner spaces");
    // player
    for (const double bad : {-1.0, 10.0, 1.5, 1e30}) {
        ExpectRejected([bad](QJsonObject& o) { o[K("player")] = bad; }, "player range");
    }
    ExpectRejected([](QJsonObject& o) { o[K("player")] = K("1"); }, "player string");
    ExpectRejected([](QJsonObject& o) { o[K("player")] = true; }, "player bool");
    ExpectAccepted([](QJsonObject& o) { o[K("player")] = 0; }, "player 0");
    ExpectAccepted([](QJsonObject& o) { o[K("player")] = 9; }, "player 9");
    // duration (the golden events end at frame 30, markers at 120)
    ExpectRejected([](QJsonObject& o) { o[K("duration_frames")] = 0; }, "duration 0");
    ExpectRejected([](QJsonObject& o) { o[K("duration_frames")] = -5; }, "negative duration");
    ExpectRejected([](QJsonObject& o) { o[K("duration_frames")] = 216001; }, "duration too long");
    ExpectRejected([](QJsonObject& o) { o[K("duration_frames")] = 120.5; }, "fractional duration");
    ExpectRejected([](QJsonObject& o) { o[K("duration_frames")] = K("120"); }, "string duration");
    ExpectRejected([](QJsonObject& o) { o[K("duration_frames")] = 100; },
                   "duration before the last marker");
    ExpectRejected([](QJsonObject& o) { o[K("duration_frames")] = 30; },
                   "duration not after the last event");
    // game_id
    ExpectAccepted([](QJsonObject& o) { o[K("game_id")] = K(""); }, "empty game id");
    ExpectAccepted([](QJsonObject& o) { o[K("game_id")] = K("0123456789abcdef"); }, "lower hex");
    ExpectRejected([](QJsonObject& o) { o[K("game_id")] = K("0100ABCDEF01200"); }, "15 digits");
    ExpectRejected([](QJsonObject& o) { o[K("game_id")] = K("0100ABCDEF0120000"); }, "17 digits");
    ExpectRejected([](QJsonObject& o) { o[K("game_id")] = K("0100ABCDEF01200G"); }, "non hex");
    ExpectRejected([](QJsonObject& o) { o[K("game_id")] = 1234; }, "numeric game id");
    // bindings
    const auto bindings = [](const char* key, const QJsonValue& value) {
        return [=](QJsonObject& o) {
            QJsonObject b = o[K("bindings")].toObject();
            b[QString::fromUtf8(key)] = value;
            o[K("bindings")] = b;
        };
    };
    ExpectAccepted(bindings("keyboard", Repeat(K("k"), 256)), "256 char shortcut");
    ExpectRejected(bindings("keyboard", Repeat(K("k"), 257)), "257 char shortcut");
    ExpectRejected(bindings("gamepad", Repeat(K("g"), 257)), "257 char gamepad shortcut");
    ExpectRejected(bindings("gamepad", 5), "gamepad shortcut type");
    ExpectAccepted(bindings("keyboard", K("")), "empty shortcut");
    // parent
    const auto parent = [](const QString& name, const QString& marker) {
        return [=](QJsonObject& o) {
            o[K("parent")] = QJsonObject{{K("name"), name}, {K("marker"), marker}};
        };
    };
    ExpectAccepted(parent(K(""), K("")), "no parent");
    ExpectRejected(parent(K("Base"), K("")), "parent without marker");
    ExpectRejected(parent(K(""), K("m")), "marker without parent");
    ExpectRejected(parent(K("Golden run"), K("m")), "own parent");
    ExpectRejected(parent(K(" Base"), K("m")), "untrimmed parent");
    ExpectRejected(parent(K("Base"), Repeat(K("m"), 129)), "long parent marker");
}

void TestEventRules() {
    ExpectRejected([](QJsonObject& o) { o[K("events")] = QJsonArray(); }, "no events");
    ExpectRejected([](QJsonObject& o) { o[K("events")] = K("none"); }, "events type");
    ExpectRejected([](QJsonObject& o) { o[K("events")] = QJsonArray{5}; }, "event type");
    ExpectRejected(EditEvent(0, [](QJsonObject& e) { e[K("frame")] = 1; }), "first frame not 0");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("frame")] = 0; }), "duplicate frame");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("frame")] = -1; }), "negative frame");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("frame")] = 120; }),
                   "frame equal to duration");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("frame")] = 30.5; }), "fractional frame");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("frame")] = K("30"); }), "string frame");
    ExpectAccepted(EditEvent(1, [](QJsonObject& e) { e[K("frame")] = 119; }), "last frame");
    ExpectRejected([](QJsonObject& o) {
        QJsonArray events = o[K("events")].toArray();
        events.prepend(events.at(1));
        o[K("events")] = events;
    }, "descending frames");

    ExpectAccepted(EditEvent(1, [](QJsonObject& e) { e[K("buttons")] = 4194303; }), "all buttons");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("buttons")] = 4194304; }), "bit 22");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("buttons")] = -1; }), "negative buttons");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("buttons")] = 1.5; }), "fraction buttons");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("buttons")] = K("1"); }), "string buttons");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("buttons")] = true; }), "bool buttons");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("buttons")] = QJsonArray{1}; }),
                   "array buttons");

    const auto set = [](const char* key, const QJsonValue& value) {
        return EditEvent(1, [=](QJsonObject& e) { e[QString::fromUtf8(key)] = value; });
    };
    ExpectRejected(set("sticks", Numbers({0, 0, 0})), "3 sticks");
    ExpectRejected(set("sticks", Numbers({0, 0, 0, 0, 0})), "5 sticks");
    ExpectRejected(set("sticks", Numbers({1.0001, 0, 0, 0})), "stick above 1");
    ExpectRejected(set("sticks", Numbers({0, 0, 0, -1.0001})), "stick below -1");
    ExpectRejected(set("sticks", QJsonArray{0, 0, K("0"), 0}), "string stick");
    ExpectRejected(set("sticks", QJsonArray{0, 0, true, 0}), "bool stick");
    ExpectRejected(set("sticks", K("0,0,0,0")), "sticks type");
    ExpectAccepted(set("sticks", Numbers({-1, 1, -1, 1})), "extreme sticks");
    ExpectRejected(set("triggers", Numbers({0})), "1 trigger");
    ExpectRejected(set("triggers", Numbers({-0.01, 0})), "trigger below 0");
    ExpectRejected(set("triggers", Numbers({0, 1.01})), "trigger above 1");
    ExpectRejected(set("motion", QJsonArray()), "no motion");
    ExpectRejected([](QJsonObject& o) {
        EditEvent(1, [](QJsonObject& e) {
            QJsonArray m = e[K("motion")].toArray();
            m.append(m.at(0));
            e[K("motion")] = m;
        })(o);
    }, "3 motion entries");
    ExpectRejected([](QJsonObject& o) {
        EditEvent(1, [](QJsonObject& e) {
            QJsonArray m = e[K("motion")].toArray();
            m.removeLast();
            e[K("motion")] = m;
        })(o);
    }, "1 motion entry");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e[K("extra")] = 0; }), "unknown event key");
    ExpectRejected(EditEvent(1, [](QJsonObject& e) { e.remove(K("sticks")); }), "missing sticks");

    const auto motion = [](const char* key, const QJsonValue& value) {
        return EditMotion(1, 1, [=](QJsonObject& m) { m[QString::fromUtf8(key)] = value; });
    };
    ExpectRejected(motion("delta_us", 0), "delta 0");
    ExpectAccepted(motion("delta_us", 1), "delta 1");
    ExpectAccepted(motion("delta_us", 1000000), "delta 1e6");
    ExpectRejected(motion("delta_us", 1000001), "delta over 1e6");
    ExpectRejected(motion("delta_us", 10.5), "fractional delta");
    ExpectRejected(motion("delta_us", K("10")), "string delta");
    ExpectRejected(motion("gyro", Numbers({0, 0})), "2 gyro values");
    ExpectRejected(motion("gyro", Numbers({100.001, 0, 0})), "gyro above 100");
    ExpectRejected(motion("accel", Numbers({0, -100.001, 0})), "accel below -100");
    ExpectRejected(motion("accel", QJsonArray{0, 0, QJsonValue::Null}), "null accel");
    ExpectRejected(EditMotion(1, 0, [](QJsonObject& m) { m[K("extra")] = 1; }),
                   "unknown motion key");
    ExpectRejected(EditMotion(1, 0, [](QJsonObject& m) { m.remove(K("accel")); }),
                   "missing accel");
}

void TestNonFiniteAndDocumentRules() {
    // JSON text cannot spell NaN/Inf, but in-memory objects can carry them: never accepted.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const double bad : {nan, inf, -inf}) {
        ExpectRejected(EditEvent(1, [bad](QJsonObject& e) {
            e[K("sticks")] = Numbers({0, bad, 0, 0});
        }), "non-finite stick");
        ExpectRejected(EditEvent(1, [bad](QJsonObject& e) {
            e[K("triggers")] = Numbers({bad, 0});
        }), "non-finite trigger");
        ExpectRejected(EditMotion(1, 0, [bad](QJsonObject& m) {
            m[K("gyro")] = Numbers({0, 0, bad});
        }), "non-finite gyro");
        ExpectRejected(EditMotion(1, 0, [bad](QJsonObject& m) {
            m[K("accel")] = Numbers({bad, 0, 0});
        }), "non-finite accel");
        ExpectRejected(EditMotion(1, 0, [bad](QJsonObject& m) { m[K("delta_us")] = bad; }),
                       "non-finite delta");
        ExpectRejected([bad](QJsonObject& o) { o[K("duration_frames")] = bad; },
                       "non-finite duration");
    }

    // The same through the typed validator.
    const auto invalid = [](const std::function<void(Document&)>& change, const char* what) {
        Document doc = GoldenDocument();
        change(doc);
        QString error;
        CHECK_MSG(!Validate(doc, error), QString::fromUtf8(what));
        CHECK_MSG(!error.isEmpty(), QString::fromUtf8(what));
    };
    invalid([&](Document& d) { d.events[1].sample.sticks[2] = static_cast<float>(nan); }, "NaN stick");
    invalid([&](Document& d) { d.events[1].sample.triggers[1] = static_cast<float>(inf); },
            "inf trigger");
    invalid([&](Document& d) { d.events[1].sample.motion[1].accel[0] = static_cast<float>(-inf); },
            "-inf accel");
    invalid([](Document& d) { d.events[1].sample.buttons = 1u << ButtonCount; }, "button bit 22");
    invalid([](Document& d) { d.events[1].sample.buttons = 0x80000000u; }, "button bit 31");
    invalid([](Document& d) { d.events[1].sample.motion[0].delta_us = 0; }, "zero delta");
    invalid([](Document& d) { d.events[1].sample.motion[0].delta_us = -5; }, "negative delta");
    invalid([](Document& d) { d.player = 10; }, "player 10");
    invalid([](Document& d) { d.player = -1; }, "player -1");
    invalid([](Document& d) { d.duration_frames = MaxFrames + 1; }, "too many frames");
    invalid([](Document& d) { d.events.clear(); }, "no events");
    invalid([](Document& d) { d.events[1].frame = d.events[0].frame; }, "equal frames");
    invalid([](Document& d) { std::swap(d.events[0], d.events[1]); }, "unordered events");
    invalid([](Document& d) { d.markers.push_back({K("start"), 1}); }, "duplicate marker");
    invalid([](Document& d) { d.markers.push_back({K(""), 1}); }, "empty marker name");
    invalid([](Document& d) { d.markers.push_back({K(" x"), 1}); }, "untrimmed marker name");
    invalid([](Document& d) { d.markers.push_back({K("late"), 121}); }, "marker after end");
    invalid([](Document& d) { d.name = QString(QChar(0xD800)); }, "lone high surrogate");
    invalid([](Document& d) { d.name = K("a") + QString(QChar(0xDC00)); }, "lone low surrogate");
    invalid([](Document& d) { d.keyboard_shortcut = K("a\nb"); }, "newline in shortcut");
    invalid([](Document& d) {
        d.events.assign(static_cast<std::size_t>(MaxEvents) + 1, d.events[0]);
        for (std::size_t i = 0; i < d.events.size(); ++i) {
            d.events[i].frame = i;
        }
        d.duration_frames = MaxFrames;
    }, "too many events");

    // Boundary values that must be accepted.
    Document edge = GoldenDocument();
    edge.player = 9;
    edge.duration_frames = MaxFrames;
    edge.events[1].frame = MaxFrames - 1;
    edge.markers = {{K("zero"), 0}, {K("end"), MaxFrames}};
    QString error;
    CHECK_MSG(Validate(edge, error), error);
    Document exactly_max_events = GoldenDocument();
    exactly_max_events.duration_frames = MaxFrames;
    exactly_max_events.events.assign(static_cast<std::size_t>(MaxEvents), Event{});
    for (std::size_t i = 0; i < exactly_max_events.events.size(); ++i) {
        exactly_max_events.events[i].frame = i;
    }
    CHECK_MSG(Validate(exactly_max_events, error), error);
}

void TestMarkerRules() {
    const auto markers = [](int count) {
        return [count](QJsonObject& o) {
            QJsonArray list;
            for (int i = 0; i < count; ++i) {
                list.append(QJsonObject{{K("name"), K("m%1").arg(i)}, {K("frame"), i % 121}});
            }
            o[K("markers")] = list;
        };
    };
    ExpectAccepted(markers(0), "no markers");
    ExpectAccepted(markers(MaxMarkers), "1024 markers");
    ExpectRejected(markers(MaxMarkers + 1), "1025 markers");
    ExpectRejected([](QJsonObject& o) { o[K("markers")] = K("x"); }, "markers type");
    ExpectRejected([](QJsonObject& o) {
        o[K("markers")] = QJsonArray{QJsonObject{{K("name"), K("a")}, {K("frame"), 1}},
                                     QJsonObject{{K("name"), K("a")}, {K("frame"), 2}}};
    }, "duplicate marker names");
    ExpectAccepted([](QJsonObject& o) {
        o[K("markers")] = QJsonArray{QJsonObject{{K("name"), K("a")}, {K("frame"), 1}},
                                     QJsonObject{{K("name"), K("A")}, {K("frame"), 2}}};
    }, "case-sensitive marker names");
    ExpectRejected([](QJsonObject& o) {
        o[K("markers")] = QJsonArray{QJsonObject{{K("name"), K("")}, {K("frame"), 1}}};
    }, "empty marker name");
    ExpectRejected([](QJsonObject& o) {
        o[K("markers")] = QJsonArray{QJsonObject{{K("name"), K("a")}, {K("frame"), 121}}};
    }, "marker beyond duration");
    ExpectAccepted([](QJsonObject& o) {
        o[K("markers")] = QJsonArray{QJsonObject{{K("name"), K("a")}, {K("frame"), 120}}};
    }, "marker at the end");
    ExpectRejected([](QJsonObject& o) {
        o[K("markers")] = QJsonArray{QJsonObject{{K("name"), K("a")}, {K("frame"), -1}}};
    }, "negative marker frame");
    ExpectRejected([](QJsonObject& o) {
        o[K("markers")] = QJsonArray{QJsonObject{{K("name"), K("a")}, {K("frame"), 1.5}}};
    }, "fractional marker frame");
    ExpectRejected([](QJsonObject& o) {
        o[K("markers")] = QJsonArray{QJsonObject{{K("name"), K("a")}}};
    }, "marker without frame");
    ExpectRejected([](QJsonObject& o) {
        o[K("markers")] = QJsonArray{QJsonObject{{K("name"), 1}, {K("frame"), 1}}};
    }, "marker name type");
    ExpectRejected([](QJsonObject& o) {
        o[K("markers")] = QJsonArray{QJsonObject{{K("name"), K("a")}, {K("frame"), 1},
                                                 {K("color"), K("red")}}};
    }, "unknown marker key");
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

void TestSaveLoad() {
    QTemporaryDir dir;
    const QString path = dir.filePath(K("macro.json"));
    const Document doc = GoldenDocument();
    QString error;
    CHECK_MSG(Save(path, doc, error), error);
    CHECK(error.isEmpty());
    Document loaded;
    CHECK_MSG(Load(path, loaded, error), error);
    CHECK(SameDocument(loaded, doc));
    CHECK(DirListing(dir.path()) == QStringList{K("macro.json")});

    // Inspect the bytes independently of Load/FromJson.
    const QByteArray bytes = ReadAll(path);
    CHECK(bytes.endsWith('\n'));
    CHECK(!bytes.contains('\r'));
    QJsonParseError parse_error;
    const QJsonDocument json = QJsonDocument::fromJson(bytes, &parse_error);
    CHECK(parse_error.error == QJsonParseError::NoError);
    CHECK(json.isObject());
    const QJsonObject root = json.object();
    CHECK(root.keys() == (QStringList{K("bindings"), K("duration_frames"), K("events"),
                                      K("format"), K("game_id"), K("markers"), K("name"),
                                      K("parent"), K("player"), K("version")}));
    CHECK(root[K("format")].toString() == K("eden-input-macro"));
    CHECK(root[K("version")].toDouble() == 1.0);
    CHECK(root[K("player")].toDouble() == 3.0);
    CHECK(root[K("events")].toArray().size() == 2);
    const QJsonObject second = root[K("events")].toArray().at(1).toObject();
    CHECK(second[K("frame")].toDouble() == 30.0);
    CHECK(second[K("buttons")].toDouble() == 2097153.0);
    CHECK(second[K("sticks")].toArray().at(0).toDouble() == 0.5);
    CHECK(second[K("motion")].toArray().at(1).toObject()[K("delta_us")].toDouble() == 1000000.0);
    CHECK(root[K("parent")].toObject()[K("marker")].toString() == K("m"));
    CHECK(root[K("bindings")].toObject()[K("keyboard")].toString() == K("Ctrl+F5"));
}

void TestOverwriteAndLinks() {
    QTemporaryDir dir;
    const QString path = dir.filePath(K("macro.json"));
    QString error;
    Document doc = GoldenDocument();
    CHECK_MSG(Save(path, doc, error), error);
    const QByteArray original = ReadAll(path);
    const QStringList listing = DirListing(dir.path());

    // An existing file is never clobbered by default.
    Document other = GoldenDocument();
    other.name = K("Other");
    CHECK(!Save(path, other, error));
    CHECK(!error.isEmpty());
    CHECK(ReadAll(path) == original);
    CHECK(DirListing(dir.path()) == listing);

    // Explicit overwrite replaces it.
    CHECK_MSG(Save(path, other, error, true), error);
    Document loaded;
    CHECK_MSG(Load(path, loaded, error), error);
    CHECK(loaded.name == K("Other"));
    CHECK(DirListing(dir.path()) == listing);

    // An invalid document creates nothing and damages nothing, even with overwrite.
    Document bad = GoldenDocument();
    bad.events.clear();
    const QByteArray before = ReadAll(path);
    CHECK(!Save(path, bad, error, true));
    CHECK(ReadAll(path) == before);
    const QString fresh = dir.filePath(K("fresh.json"));
    CHECK(!Save(fresh, bad, error));
    CHECK(!QFileInfo::exists(fresh));
    CHECK(DirListing(dir.path()) == listing);

    // A directory in the way.
    CHECK(QDir(dir.path()).mkdir(K("folder")));
    CHECK(!Save(dir.filePath(K("folder")), doc, error, true));
    CHECK(!Save(dir.filePath(K("folder")), doc, error, false));
    CHECK(QFileInfo(dir.filePath(K("folder"))).isDir());

    // The parent directory is never created and nothing is written elsewhere.
    const QString nested = dir.filePath(K("missing/sub/macro.json"));
    CHECK(!Save(nested, doc, error));
    CHECK(!error.isEmpty());
    CHECK(!QFileInfo::exists(dir.filePath(K("missing"))));
    CHECK(!Save(QString(), doc, error));

#ifdef Q_OS_UNIX
    // A symbolic link is never written through, with or without overwrite.
    const QString victim = dir.filePath(K("victim.txt"));
    CHECK(WriteAll(victim, QByteArray("precious")));
    const QString link = dir.filePath(K("link.json"));
    CHECK(QFile::link(victim, link));
    CHECK(!Save(link, doc, error, false));
    CHECK(!Save(link, doc, error, true));
    CHECK(ReadAll(victim) == QByteArray("precious"));
    CHECK(QFileInfo(link).isSymLink());
    // Dangling link: the target must not be created.
    const QString target = dir.filePath(K("not-yet.json"));
    const QString dangling = dir.filePath(K("dangling.json"));
    CHECK(QFile::link(target, dangling));
    CHECK(!Save(dangling, doc, error, true));
    CHECK(!Save(dangling, doc, error, false));
    CHECK(!QFileInfo::exists(target));
    CHECK(DirListing(dir.path()).contains(K("link.json")));

    // A directory we cannot write into fails cleanly without leaving temporary files.
    if (geteuid() != 0) {
        CHECK(QDir(dir.path()).mkdir(K("readonly")));
        const QString locked = dir.filePath(K("readonly"));
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        CHECK(!Save(locked + K("/macro.json"), doc, error));
        CHECK(!error.isEmpty());
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                          QFileDevice::ExeOwner);
        CHECK(DirListing(locked).isEmpty());
    }
#endif
}

void TestLoadFailures() {
    QTemporaryDir dir;
    const auto expect_failure = [&](const QByteArray& content, const char* what) {
        const QString path = dir.filePath(K("bad.json"));
        CHECK(WriteAll(path, content));
        Document out = Sentinel();
        QString error;
        CHECK_MSG(!Load(path, out, error), QString::fromUtf8(what));
        CHECK_MSG(!error.isEmpty(), QString::fromUtf8(what));
        CHECK_MSG(IsSentinel(out), QString::fromUtf8(what));
    };
    const QByteArray good(GoldenText);
    {
        Document out;
        QString error;
        CHECK(WriteAll(dir.filePath(K("good.json")), good));
        CHECK_MSG(Load(dir.filePath(K("good.json")), out, error), error);
        CHECK(SameDocument(out, GoldenDocument()));
    }
    expect_failure(QByteArray(), "empty file");
    expect_failure(QByteArray("   \n"), "whitespace only");
    expect_failure(good.left(good.size() / 2), "truncated");
    expect_failure(good + QByteArray(" trailing"), "trailing garbage");
    expect_failure(QByteArray("[1, 2, 3]"), "array root");
    expect_failure(QByteArray("42"), "scalar root");
    expect_failure(QByteArray("not json at all"), "garbage");
    expect_failure(QByteArray("{\"format\": \"eden-input-macro\"}"), "missing keys");
    QByteArray bad_utf8 = good;
    bad_utf8.replace("Golden run", "Gold\xFF\xFEn");
    expect_failure(bad_utf8, "invalid UTF-8");
    QByteArray lone_surrogate = good;
    lone_surrogate.replace("Golden run", "Gold\\ud800n");
    expect_failure(lone_surrogate, "lone surrogate escape");
    QByteArray nul_escape = good;
    nul_escape.replace("Golden run", "Gold\\u0000n");
    expect_failure(nul_escape, "NUL escape");
    QByteArray huge_number = good;
    huge_number.replace("\"duration_frames\": 120", "\"duration_frames\": 1e999");
    expect_failure(huge_number, "overflowing number");
    QByteArray not_a_number = good;
    not_a_number.replace("\"duration_frames\": 120", "\"duration_frames\": NaN");
    expect_failure(not_a_number, "NaN token");
    QByteArray wrong_version = good;
    wrong_version.replace("\"version\": 1", "\"version\": 2");
    expect_failure(wrong_version, "unsupported version");

    // Not a file, or not there.
    Document out = Sentinel();
    QString error;
    CHECK(!Load(dir.filePath(K("absent.json")), out, error));
    CHECK(!error.isEmpty());
    CHECK(!Load(dir.path(), out, error));
    CHECK(!Load(QString(), out, error));
    CHECK(IsSentinel(out));

    // Size limit: a file of exactly MaxFileBytes is read, one byte more is rejected without
    // parsing (the content stays valid JSON, so only the size check can reject it).
    const QString big = dir.filePath(K("big.json"));
    {
        QFile file(big);
        CHECK(file.open(QIODevice::WriteOnly));
        file.write(good);
        const QByteArray padding(1024 * 1024, ' ');
        qint64 remaining = MaxFileBytes - good.size();
        while (remaining > 0) {
            const qint64 chunk = std::min<qint64>(remaining, padding.size());
            CHECK(file.write(padding.constData(), chunk) == chunk);
            remaining -= chunk;
        }
        CHECK(file.size() == MaxFileBytes);
    }
    Document at_limit;
    CHECK_MSG(Load(big, at_limit, error), error);
    CHECK(SameDocument(at_limit, GoldenDocument()));
    {
        QFile file(big);
        CHECK(file.open(QIODevice::Append));
        CHECK(file.write(" ", 1) == 1);
    }
    CHECK(QFileInfo(big).size() == MaxFileBytes + 1);
    out = Sentinel();
    CHECK(!Load(big, out, error));
    CHECK(!error.isEmpty());
    CHECK(IsSentinel(out));
    QFile::remove(big);
}

void TestUnicode() {
    const QString combining = K("e\U00000301clair \U0000062E \U0001F3AE\U0000200D\U00002728 \U000030DE\U000030AF\U000030ED");
    Document doc = GoldenDocument();
    doc.name = combining;
    doc.parent_name = K("\U000089AA\U000030DE\U000030AF\U000030ED");
    doc.parent_marker = K("\U000030DE\U000030FC\U000030AB\U000030FC \U0001F3C1");
    doc.keyboard_shortcut = K("Ctrl+\U000000C9");
    doc.markers = {{K("\U000030DE\U000030FC\U000030AB\U000030FC \U0001F3C1"), 5}, {K("\U000000E9"), 6},
                   {K("e\U00000301"), 7}};
    QString error;
    CHECK_MSG(Validate(doc, error), error);

    QTemporaryDir dir;
    CHECK(QDir(dir.path()).mkdir(K("\U000030C7\U000030A3\U000030EC\U000030AF\U000030C8\U000030EA \U000000F1")));
    const QString path =
        dir.filePath(K("\U000030C7\U000030A3\U000030EC\U000030AF\U000030C8\U000030EA \U000000F1/\U000030DE\U000030AF\U000030ED-\U0001F3AE.json"));
    CHECK_MSG(Save(path, doc, error), error);
    CHECK(QFileInfo::exists(path));
    Document loaded;
    CHECK_MSG(Load(path, loaded, error), error);
    CHECK(SameDocument(loaded, doc));
    CHECK(!Save(path, doc, error));
    CHECK_MSG(Save(path, doc, error, true), error);
    // The precomposed and the combining spellings of e-acute are different, distinct marker names (no normalization).
    CHECK(loaded.markers[1].name != loaded.markers[2].name);

    // 128 supplementary-plane characters are 256 UTF-16 units but only 128 characters.
    const QString emoji = K("\U0001F3AE");
    Document longest = GoldenDocument();
    longest.name = Repeat(emoji, 128);
    CHECK_MSG(Validate(longest, error), error);
    longest.name = Repeat(emoji, 129);
    CHECK(!Validate(longest, error));
    longest.name = Repeat(K("\U000000E9"), 128);
    CHECK(Validate(longest, error));
    longest.name = Repeat(K("\U000000E9"), 129);
    CHECK(!Validate(longest, error));
    Document shortcut = GoldenDocument();
    shortcut.gamepad_shortcut = Repeat(emoji, 256);
    CHECK_MSG(Validate(shortcut, error), error);
    shortcut.gamepad_shortcut = Repeat(emoji, 257);
    CHECK(!Validate(shortcut, error));

    // Whitespace beyond ASCII is whitespace for the trimmed rule.
    Document spaced = GoldenDocument();
    spaced.name = K("\U00002003em space");
    CHECK(!Validate(spaced, error));
    spaced.name = K("ok\U00003000inside");
    CHECK(Validate(spaced, error));
}

// ---------------------------------------------------------------------------------------------
// Prefix (branch) creation
// ---------------------------------------------------------------------------------------------

std::vector<quint64> Frames(const Document& doc) {
    std::vector<quint64> frames;
    for (const Event& event : doc.events) {
        frames.push_back(event.frame);
    }
    return frames;
}

QStringList MarkerNames(const Document& doc) {
    QStringList names;
    for (const Marker& marker : doc.markers) {
        names.append(marker.name);
    }
    return names;
}

void TestPrefixCuts() {
    const Document source = MakeSourceDocument();
    QString error;
    CHECK_MSG(Validate(source, error), error);

    const auto make = [&](const char* marker, Document& out) {
        QString message;
        const bool ok = MakePrefix(source, QString::fromUtf8(marker), K("Branch"), out, message);
        CHECK_MSG(ok, message);
        return ok;
    };

    // Common invariants for every cut: independent copy, parent metadata, no bindings.
    const auto common = [&](const Document& out, quint64 cut) {
        CHECK(out.name == K("Branch"));
        CHECK(out.game_id == source.game_id);
        CHECK(out.player == source.player);
        CHECK(out.duration_frames == cut);
        CHECK(out.parent_name == source.name);
        CHECK(out.keyboard_shortcut.isEmpty());
        CHECK(out.gamepad_shortcut.isEmpty());
        for (const Event& event : out.events) {
            CHECK(event.frame < cut);
        }
        for (const Marker& marker : out.markers) {
            CHECK(marker.frame <= cut);
        }
        for (quint64 frame = 0; frame < cut; ++frame) {
            CHECK(SampleAt(out, frame) == SampleAt(source, frame));
        }
    };

    // The event exactly at the cut belongs to the tail, not the prefix.
    Document out;
    if (make("boss", out)) {
        common(out, 25);
        CHECK(out.parent_marker == K("boss"));
        CHECK((Frames(out) == std::vector<quint64>{0, 10}));
        CHECK((MarkerNames(out) == QStringList{K("m0"), K("ten"), K("eleven"), K("boss")}));
        CHECK_MSG(Validate(out, error), error);
        // Mutating the copy cannot reach the source.
        const Document before = source;
        out.events[0].sample.buttons = 0x3FFFFF;
        out.markers[0].name = K("changed");
        CHECK(SameDocument(source, before));
    }
    if (make("ten", out)) {
        common(out, 10);
        CHECK((Frames(out) == std::vector<quint64>{0}));
        CHECK((MarkerNames(out) == QStringList{K("m0"), K("ten")}));
    }
    if (make("eleven", out)) {
        common(out, 11);
        CHECK((Frames(out) == std::vector<quint64>{0, 10}));
    }
    if (make("late", out)) {
        common(out, 100);
        CHECK((Frames(out) == std::vector<quint64>{0, 10, 25}));
        CHECK(MarkerNames(out).contains(K("Boss")));
    }

    // The very end clones everything.
    if (make("end", out)) {
        common(out, 200);
        CHECK(Frames(out) == Frames(source));
        CHECK(MarkerNames(out) == MarkerNames(source));
        CHECK_MSG(Validate(out, error), error);
        for (quint64 frame : {0u, 50u, 199u, 500u}) {
            CHECK(SampleAt(out, frame) == SampleAt(source, frame));
        }
    }

    // A marker at frame 0 gives an empty draft that is only a valid macro once frames exist.
    QTemporaryDir dir;
    const QString path = dir.filePath(K("draft.json"));
    if (make("m0", out)) {
        CHECK(out.events.empty());
        CHECK(out.duration_frames == 0);
        CHECK(out.parent_name == source.name && out.parent_marker == K("m0"));
        CHECK((MarkerNames(out) == QStringList{K("m0")}));
        CHECK(!Validate(out, error));
        CHECK(!Save(path, out, error));
        CHECK(!QFileInfo::exists(path));
        // The recorder appends the new tail and extends the duration.
        Event recorded;
        recorded.frame = 0;
        recorded.sample.buttons = 0x10;
        out.events.push_back(recorded);
        out.duration_frames = 30;
        CHECK_MSG(Validate(out, error), error);
        CHECK_MSG(Save(path, out, error), error);
        Document loaded;
        CHECK_MSG(Load(path, loaded, error), error);
        CHECK(SameDocument(loaded, out));
    }

    // Recording a tail after a middle cut gives a valid, self-contained branch.
    if (make("boss", out)) {
        Event tail;
        tail.frame = 25;
        tail.sample.buttons = 0x4;
        out.events.push_back(tail);
        out.duration_frames = 80;
        CHECK_MSG(Validate(out, error), error);
        CHECK(SampleAt(out, 24) == SampleAt(source, 24));
        CHECK(SampleAt(out, 25).buttons == 0x4);
        CHECK(SampleAt(out, 79).buttons == 0x4);
        // A branch of a branch names its direct parent.
        Document grand;
        CHECK(MakePrefix(out, K("boss"), K("Grandchild"), grand, error));
        CHECK(grand.parent_name == K("Branch") && grand.parent_marker == K("boss"));
        CHECK(grand.duration_frames == 25);
    }
}

void TestPrefixFailures() {
    const Document source = MakeSourceDocument();
    QString error;
    const auto expect_failure = [&](const Document& from, const QString& marker, const QString& name,
                                    const char* what) {
        Document out = Sentinel();
        CHECK_MSG(!MakePrefix(from, marker, name, out, error), QString::fromUtf8(what));
        CHECK_MSG(!error.isEmpty(), QString::fromUtf8(what));
        CHECK_MSG(IsSentinel(out), QString::fromUtf8(what));
    };
    expect_failure(source, K("nope"), K("B"), "unknown marker");
    expect_failure(source, K("BOSS"), K("B"), "marker names are case-sensitive");
    expect_failure(source, K(""), K("B"), "empty marker name");
    expect_failure(source, K("boss"), K(""), "empty new name");
    expect_failure(source, K("boss"), K(" B"), "untrimmed new name");
    expect_failure(source, K("boss"), Repeat(K("n"), 129), "overlong new name");
    expect_failure(source, K("boss"), source.name, "new name equals source name");
    Document broken = source;
    std::swap(broken.events[0], broken.events[1]);
    expect_failure(broken, K("boss"), K("B"), "invalid source events");
    Document empty = source;
    empty.events.clear();
    expect_failure(empty, K("boss"), K("B"), "source without events");

    // The source is never modified by failures or successes.
    Document untouched = source;
    Document out;
    CHECK(MakePrefix(untouched, K("late"), K("B"), out, error));
    CHECK(!MakePrefix(untouched, K("zzz"), K("B"), out, error));
    CHECK(SameDocument(untouched, source));

    // The destination may be the source itself.
    Document alias = source;
    CHECK_MSG(MakePrefix(alias, K("boss"), K("Alias"), alias, error), error);
    CHECK(alias.name == K("Alias") && alias.parent_name == source.name &&
          alias.duration_frames == 25);
    CHECK(Frames(alias) == (std::vector<quint64>{0, 10}));
}

// ---------------------------------------------------------------------------------------------
// Sample lookup and constants
// ---------------------------------------------------------------------------------------------

void TestSampleAt() {
    const Document doc = MakeSourceDocument(); // events at 0, 10, 25, 100 with buttons 1, 2, 4, 8
    const struct {
        quint64 frame;
        quint32 buttons;
    } cases[] = {{0, 1}, {1, 1}, {9, 1}, {10, 2}, {11, 2}, {24, 2}, {25, 4}, {26, 4}, {99, 4},
                 {100, 8}, {101, 8}, {199, 8}, {200, 8}, {216000, 8},
                 {std::numeric_limits<quint64>::max(), 8}};
    for (const auto& item : cases) {
        CHECK_MSG(SampleAt(doc, item.frame).buttons == item.buttons,
                  K("frame %1").arg(item.frame));
    }
    CHECK(&SampleAt(doc, 10) == &doc.events[1].sample);

    const Document empty;
    CHECK(SampleAt(empty, 0) == Sample{});
    CHECK(SampleAt(empty, 12345) == Sample{});
    CHECK(&SampleAt(empty, 0) == &SampleAt(empty, 9));

    Document single = GoldenDocument();
    single.events.resize(1);
    CHECK(SampleAt(single, 0) == single.events[0].sample);
    CHECK(SampleAt(single, 119) == single.events[0].sample);
}

void TestButtonNamesAndConstants() {
    const QStringList expected{K("a"),        K("b"),      K("x"),    K("y"),       K("lstick"),
                               K("rstick"),   K("l"),      K("r"),    K("zl"),      K("zr"),
                               K("plus"),     K("minus"),  K("left"), K("up"),      K("right"),
                               K("down"),     K("sl_left"), K("sr_left"), K("home"), K("capture"),
                               K("sl_right"), K("sr_right")};
    CHECK(ButtonNames() == expected);
    CHECK(ButtonNames().size() == ButtonCount);
    CHECK(ButtonCount == 22);
    CHECK(ButtonMask == 0x3FFFFFu);
    CHECK(MaxFrames == 216000);
    CHECK(MaxEvents == 216000);
    CHECK(MaxFileBytes == 67108864);
    CHECK(FormatName() == K("eden-input-macro"));

    // The names must agree with the controller code that will consume the bit positions.
    QFile source(QString::fromUtf8(EDEN_CONTROL_INPUT_SOURCE));
    CHECK(source.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(source.readAll());
    const qsizetype begin = text.indexOf(K("ButtonNames{"));
    const qsizetype end = begin < 0 ? -1 : text.indexOf(K("};"), begin);
    CHECK(begin >= 0 && end > begin);
    if (begin >= 0 && end > begin) {
        QStringList found;
        const QRegularExpression pattern(K("QStringLiteral\\(\"([a-z_]+)\"\\)"));
        auto matches = pattern.globalMatch(text.mid(begin, end - begin));
        while (matches.hasNext()) {
            found.append(matches.next().captured(1));
        }
        CHECK_MSG(found == expected, found.join(K(",")));
    }
}

// ---------------------------------------------------------------------------------------------
// Corpus for the independent validator
// ---------------------------------------------------------------------------------------------

QJsonArray FlatInts(const Event& event) {
    return QJsonArray{static_cast<qint64>(event.frame), static_cast<qint64>(event.sample.buttons),
                      event.sample.motion[0].delta_us, event.sample.motion[1].delta_us};
}

QJsonArray FlatFloats(const Event& event) {
    QJsonArray floats;
    const Sample& s = event.sample;
    for (const float v : s.sticks) {
        floats.append(static_cast<double>(v));
    }
    for (const float v : s.triggers) {
        floats.append(static_cast<double>(v));
    }
    for (const Motion& m : s.motion) {
        for (const float v : m.gyro) {
            floats.append(static_cast<double>(v));
        }
        for (const float v : m.accel) {
            floats.append(static_cast<double>(v));
        }
    }
    return floats;
}

void WriteCorpus(const QString& directory) {
    CHECK(QDir().mkpath(directory));
    Document branch;
    QString error;
    const Document source = MakeSourceDocument();
    CHECK(MakePrefix(source, K("boss"), K("Branch \U0001F33F"), branch, error));
    Event tail;
    tail.frame = 25;
    tail.sample.buttons = 0x3FFFFF;
    tail.sample.sticks = {0.123456789f, -0.987654321f, 1.0f / 3.0f, -1.0f};
    tail.sample.motion[1].gyro = {0.1f, -99.99999f, 1.4e-45f};
    branch.events.push_back(tail);
    branch.duration_frames = 60;

    Document unicode = GoldenDocument();
    unicode.name = K("e\U00000301clair \U000030DE\U000030AF\U000030ED \U0001F3AE");
    unicode.markers = {{K("\U000030DE\U000030FC\U000030AB\U000030FC"), 1}, {K("\"quote\" \\ back/slash"), 2}};

    const std::pair<QString, Document> files[] = {
        {K("golden.json"), GoldenDocument()},
        {K("source.json"), source},
        {K("branch.json"), branch},
        {K("\U000030DE\U000030AF\U000030ED-\U0001F3AE.json"), unicode},
    };
    QJsonArray manifest;
    for (const auto& [file, doc] : files) {
        CHECK_MSG(Save(QDir(directory).filePath(file), doc, error), error);
        QJsonArray ints;
        QJsonArray floats;
        for (const Event& event : doc.events) {
            ints.append(FlatInts(event));
            floats.append(FlatFloats(event));
        }
        QJsonArray markers;
        for (const Marker& marker : doc.markers) {
            markers.append(QJsonArray{marker.name, static_cast<qint64>(marker.frame)});
        }
        manifest.append(QJsonObject{
            {K("file"), file},
            {K("name"), doc.name},
            {K("game_id"), doc.game_id},
            {K("player"), doc.player},
            {K("duration_frames"), static_cast<qint64>(doc.duration_frames)},
            {K("keyboard"), doc.keyboard_shortcut},
            {K("gamepad"), doc.gamepad_shortcut},
            {K("parent_name"), doc.parent_name},
            {K("parent_marker"), doc.parent_marker},
            {K("markers"), markers},
            {K("event_ints"), ints},
            {K("event_floats"), floats},
        });
    }
    CHECK(WriteAll(QDir(directory).filePath(K("manifest.json")),
                   QJsonDocument(QJsonObject{{K("files"), manifest}}).toJson()));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: macro_file_test <output-dir>\n");
        return 2;
    }
    TestGolden();
    TestFloatRoundTrip();
    TestRootRules();
    TestScalarFieldRules();
    TestEventRules();
    TestNonFiniteAndDocumentRules();
    TestMarkerRules();
    TestSaveLoad();
    TestOverwriteAndLinks();
    TestLoadFailures();
    TestUnicode();
    TestPrefixCuts();
    TestPrefixFailures();
    TestSampleAt();
    TestButtonNamesAndConstants();
    WriteCorpus(QString::fromUtf8(argv[1]));

    std::printf("%d checks, %d failed\n", total_checks, failed_checks);
    return failed_checks == 0 ? 0 : 1;
}
