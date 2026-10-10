# Working contract for implementation and review

Local JSON lines, one request per line:
`{"id":"arbitrary-json-id","method":"ui.tree","params":{}}`.
Response `{"id":...,"ok":true,"result":...}` or
`{"id":...,"ok":false,"error":{"code":"...","message":"..."}}`.
Requests <=1 MiB; responses <=16 MiB. Strict types and finite numeric values.

Qt server in `src/yuzu/control_server.{h,cpp}` (independent of emulator core):
`class ControlServer : public QObject` with
- `bool Listen(const QString& socket, const QString& log_path, QString* error)`
- `void SetHandler(std::function<QJsonObject(const QString&, const QJsonObject&)> handler)`
- `QJsonObject Enqueue(const QString& method, std::function<QJsonObject()> operation)`
  returns an ok envelope containing accepted/job id; operation later returns an envelope.
- `static QJsonObject Success(const QJsonValue& result = QJsonObject{})`
- `static QJsonObject Error(const QString& code, const QString& message)`

Root primary owns `main.cpp`, `main_window.h`, CMake integration,
`control_adapter.{h,cpp}` (friend adapter to MainWindow), Python tools and build.
Fable reviewed the server and Qt tests; Grok reviewed input/HID wiring and tests.

Server provides ping, schema, ui.tree/actions/trigger/click/set/key/context/items/select,
ui.close, ui.screenshot, jobs.list/get. Selectors support a session object id or unique objectName.
Tree includes IDs, parent IDs, class, objectName, visible/enabled, text, properties,
supported operations. All widgets and associated QActions, including open context menus.
Dynamic object IDs must not alias a new object after destruction. Validate all UI mutations,
normal enabled and modal state. Open dialogs stay controllable. Nonnative dialogs in
automation mode. Deferred jobs report running/done/error; no waiting for dialog closure
before sending acceptance. Do not bind TCP or remove a live socket.

Adapter provides status, game.load/pause/resume/stop, settings.list/get/set/save,
log.filter, input.* and optional renderer screenshot. Handler returns error unknown_method
when unsupported. Core/settings commands are unavailable until MainWindow is constructed;
generic Qt server starts before construction for startup dialogs.

CLI implemented at tools/edenctl.py with generic call and JSON-line attach. Convenience
input.button params: player 0..9, button name, state press/release/tap, duration_ms;
input.stick: player, stick left/right, x/y; input.motion (left/right/both);
input.trigger; input.ring; input.touch; input.reset; input.state.
Button tap and held inputs must expire via bounded lease, without later timer releasing a
newer press (one replaceable deadline per input). Runtime data changes on GUI thread. Discover input names,
axis ranges and sensor units in schema. Reset injected inputs on shutdown/expiry.

Bounded Linux launcher tools/eden-run.py uses systemd user cgroup MemoryMax (8 GiB default),
MemorySwapMax=0, RuntimeMaxSec (600s), TimeoutStopSec=5; external ping watchdog (startup90s,
hang30s). Cgroup catches descendants, survives supervisor loss with hard lifetime cap.
Logs rotate, latest screenshot optional, manifest and process/service diagnostics retained.

Build dependencies available without root under /home/intpa/eden-build-deps/root;
runner `/usr/bin/python3 /home/intpa/eden-build-deps/run.py COMMAND ARGS...` sets env.
Full Eden build succeeded with one compiler job and a 4 GiB cgroup cap.
Standalone Qt tests may use root/usr CMAKE_PREFIX_PATH and runner for relocated Qt.
