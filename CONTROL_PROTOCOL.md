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

On the experimental `ns2` branch, `status.result.emulation` reports:

- `generation`: `none`, `switch`, or `switch2`, from the loaded application's metadata.
- `compiled_cpu_cores`: the build's core capacity; this is not a runtime core-count setter.
- `multicore`: the current multicore setting.
- `memory_layout`, `memory_layout_value`, and `global_memory_layout_value`: the
  effective canonical layout and effective/global enum values.
- `memory_layout_automatic`: true only while powered on when this load selected
  the 12 GiB layout automatically. A manually selected 12 GiB layout is not automatic.
- `generation_forces_auto_stub`: a Switch 2 caller receives upstream's automatic
  stub policy even when the generic auto-stub setting is false. Other settings,
  service-specific policies, and startup environment overrides can also enable stubs.

Use `settings.list/get/set` for existing core settings. Stop the game before
changing startup-only memory or multicore options. These fields expose execution
state; they do not establish game compatibility. See [NX2 usage](docs/user/NX2.md)
for startup diagnostics and frontend/platform limitations.

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

## GIF recording

`recording.start`, `recording.stop`, and `recording.status` capture animated game GIFs.
See [GIF recording](docs/user/GIFRecording.md) for GUI and CLI usage, limits, and output semantics.

## Input macros

`macro.record`, `macro.marker`, `macro.record_from`, and `macro.stop` create named
input recordings and independent branches. `macro.play` starts playback immediately
by name or absolute file path; `macro.status` reports its progress. `macro.list`,
`macro.inspect`, `macro.bind`, and `macro.shortcuts` manage the library and keyboard
and gamepad bindings. These methods are separate from GIF `recording.*` methods.
See [Input macros](docs/user/InputMacros.md) for GUI/CLI examples and frame timing.
