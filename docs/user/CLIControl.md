# CLI control and bounded diagnostic runs

The Qt frontend has an opt-in local control endpoint. It discovers the live
application's menus, widgets, dialogs and scalar settings, and accepts input through
Eden's virtual gamepad. The endpoint is disabled during normal launches.

Control is independent of the desktop keyboard and mouse. Commands travel over
the local socket to Qt objects or Eden's virtual controller; `ui.key` and
`ui.click` dispatch events inside this application without global input injection
or moving the system pointer. An agent can discover and operate menus, settings,
dialogs and game inputs while the user works elsewhere. Normal disabled controls,
modal state and controller compatibility still apply; every individual UI option
has not been exhaustively tested.

On Linux, use the bounded launcher:

```powershell
/usr/bin/python3 tools/eden-run.py --memory-gib 8 --runtime 600 --output /tmp/eden-run-1 -- /absolute/path/to/eden
```

Add `--background` before `--output` to work without bringing Eden to the
foreground. Direct launches use `--control-background` with `--control-socket`.
This mode sets nonactivating windows below other applications, suppresses Eden's
explicit raises and disables Qt's desktop keyboard/mouse grabs on X11/Xwayland,
including popup menus. Menus, modal dialogs, file pickers and gamepad input remain
available through the socket. `ping.result.background` confirms the mode.
Auto-pause on loss of focus is bypassed for this launch; saved preferences are
unchanged. Restart without the flag for ordinary interactive window focus.
Fullscreen requests in background mode use a screen-sized borderless window,
avoiding the compositor's exclusive-fullscreen activation policy.

Window screenshots work while other applications cover Eden; use `target: all`
or a window ID. `target: screen` captures the desktop instead. Third-party
programs (e.g. an independently launched RenderDoc UI, file manager or browser)
are separate processes and do not inherit Eden's window policy. Background mode
has been verified on Linux X11/Xwayland; other display backends are not validated.

Use a new output directory for each run. The launcher prints the socket path and
writes `run.json`. It requires a systemd user session and verifies the memory,
swap and runtime properties. Memory includes descendant processes; device-local
GPU memory is outside this accounting. A single compiler job is recommended when
building on a machine with limited available memory.

Defaults are an 8 GiB memory limit, no swap, a 600-second total lifetime, a
90-second endpoint startup timeout and a 30-second unresponsive-GUI timeout.
The independent supervisor also detects a loaded, running game without frames
or shader work for 120 seconds. `--stall-timeout 0` disables that heuristic.
Paused games and modal dialogs are excluded. `RuntimeMaxSec` remains effective
if the supervisor exits unexpectedly.

The launcher uses X11/Xwayland by default so window captures can include native
Vulkan/OpenGL surfaces. An explicit `QT_QPA_PLATFORM` environment variable takes
precedence. It inherits display authentication from the systemd user session.
Use separate `XDG_CONFIG_HOME`, `XDG_DATA_HOME` and `XDG_CACHE_HOME` directories
when a diagnostic run should have its own emulator profile.

For a manually managed process:

```powershell
eden --control-socket /tmp/eden-control.sock --control-log /tmp/eden-control.jsonl --control-verbose
```

Use a private directory for the socket and diagnostics. Only the same local user
can connect. The server refuses a live socket or an occupied non-socket path and
can reclaim a dead socket after an unclean exit. There is no TCP listener. Normal enabled states and modal dialogs
still apply. File dialogs use Qt widgets in control mode so they are discoverable.

## Inspect and attach

```powershell
$env:EDEN_CONTROL_SOCKET='/tmp/eden-run-1/control.sock'
/usr/bin/python3 tools/edenctl.py ping
/usr/bin/python3 tools/edenctl.py status
/usr/bin/python3 tools/edenctl.py schema
/usr/bin/python3 tools/edenctl.py actions
/usr/bin/python3 tools/edenctl.py tree
/usr/bin/python3 tools/edenctl.py settings
```

`call METHOD JSON` exposes every protocol method. `attach` reads one JSON request
per stdin line and prints one JSON response per line. Each request has a total
deadline (`--timeout`, default 5 seconds). Responses are machine-readable JSON;
the client exits nonzero for errors.

Objects have a session ID such as `o42`, an object name, parent and window IDs,
type, availability and readable scalar properties. Prefer a unique object name
for repeatable scripts; use the session ID for unnamed or duplicate objects.
IDs expire when a dialog or context menu is destroyed. Rediscover after opening
a dialog, submenu, context menu or changing the game list.

```powershell
/usr/bin/python3 tools/edenctl.py trigger action_Configure
/usr/bin/python3 tools/edenctl.py tree
/usr/bin/python3 tools/edenctl.py call ui.tree '{"target":"o42"}'
/usr/bin/python3 tools/edenctl.py --wait 3 set o57 checked true
/usr/bin/python3 tools/edenctl.py --wait 3 click o61
```

Use names actually returned by discovery. `ui.items` lists a tree/list/table's
rows; `ui.select` accepts `row`, `column`, optional ancestor `parents`, `value`,
`check_state`, `expand` and `activate`. `ui.context` opens the context menu at
an item or widget coordinate. `ui.key` sends a Qt key chord such as `Return` or
`Ctrl+A`. Configuration controls retain their ordinary Apply/OK/Cancel behavior.

UI and game operations return `{accepted:true,job:"…"}` before they execute.
Inspect `jobs.get` with `{"job":"…"}` or use the client's `--wait SECONDS`.
A running job can be waiting inside a modal dialog: inspect `ui.tree` and operate
that dialog from another command. A client timeout does not cancel the operation;
inspect the job before retrying. The server retains at most 128 jobs, with at most
32 outstanding operations, 32 client connections, 1 MiB requests and 16 MiB replies.

## Game and settings

```powershell
/usr/bin/python3 tools/edenctl.py call game.load '{"path":"/absolute/path/to/game.xci"}'
/usr/bin/python3 tools/edenctl.py call game.pause
/usr/bin/python3 tools/edenctl.py call game.resume
/usr/bin/python3 tools/edenctl.py call game.stop
/usr/bin/python3 tools/edenctl.py call settings.get '{"key":"core.volume"}'
/usr/bin/python3 tools/edenctl.py call settings.set '{"key":"core.volume","value":50}'
/usr/bin/python3 tools/edenctl.py call settings.save
```

Profile selection, load failures and shutdown confirmation can open dialogs.
`status` reports running/loading/shutdown state, FPS, shader work and the age of
the latest sampled frame. These values are telemetry, not a guarantee that a
guest is making useful progress.

Settings keys come from `settings.list`; numeric enums include their current
canonical name and numeric value. Updates are type/range checked. Non-runtime
settings require stopping emulation. A switchable setting updates its currently
effective global or custom value. UI preference values may require their normal
UI action or a restart to take visual effect; use the dialog controls for the
same immediate behavior as a human. Complex options such as game directories,
input profiles and hotkey bindings are available through their configuration UI.

## Controller input

```powershell
/usr/bin/python3 tools/edenctl.py call input.schema
/usr/bin/python3 tools/edenctl.py button a
/usr/bin/python3 tools/edenctl.py button zr --state press --lease-ms 2000
/usr/bin/python3 tools/edenctl.py stick left 0.5 0 --lease-ms 500
/usr/bin/python3 tools/edenctl.py motion --gyro 0 0 0 --accel 0 0 -1 --delta-us 10000
/usr/bin/python3 tools/edenctl.py motion --sensor right --gyro 0 0.5 0 --accel 0 0 -1
/usr/bin/python3 tools/edenctl.py analog-trigger left 0.75 --lease-ms 500
/usr/bin/python3 tools/edenctl.py ring 0.3 --lease-ms 500
/usr/bin/python3 tools/edenctl.py touch press --finger 0 --x 0.5 --y 0.5
/usr/bin/python3 tools/edenctl.py release
```

Player indices 0–7 are players 1–8, 8 is handheld and 9 is the other controller
slot. Buttons include both Joy-Con SL/SR pairs, Home and Capture. Positive stick
Y points up; touch coordinates start at the top left. Consult `input.schema`
for the exact supported inputs and units.

GameCube analog triggers and Ring-Con flex have separate commands. Motion can
target either Joy-Con sensor or both; handheld left motion also feeds the console
IMU. Input replies distinguish acceptance, observed frontend state and known
gates such as disconnected or incompatible controller styles. They do not claim
the guest has consumed a sample; HID polling and sensor activation happen later.

Held input expires after a bounded lease (1–10000 ms, default 1000 ms). Renew the
same input to keep holding it. Taps default to 100 ms. A new press replaces the
previous expiry deadline, so an old tap cannot release a newer press. Reset and
emulation shutdown release injected input. Controller style, connectivity,
mapping mode and game-enabled sensors determine what the guest can receive.
Expiry is checked on a 20 ms GUI timer; it is not a precise frame scheduler.

## Screenshots and diagnostics

Screenshot dimensions are device pixels. Combined captures account for display
scaling; native X11 capture and composition were verified at 100% and 200%.

```powershell
/usr/bin/python3 tools/edenctl.py screenshot /tmp/eden.png --target active
/usr/bin/python3 tools/edenctl.py screenshot /tmp/window.png --target o42
/usr/bin/python3 tools/edenctl.py screenshot /tmp/application.png --target all
/usr/bin/python3 tools/edenctl.py call log.filter '{"filter":"*:Info Frontend:Debug Render:Debug"}'
```

Capture returns the PNG path, dimensions, backend and whether the pixels are
uniform. `active` captures the active application window or dialog; an object ID
selects a specific widget. `all` combines visible application windows and dialogs.
On X11/Xwayland, capture reads the window's own pixels, including native GPU
surfaces and the last presented frame while paused. Native Wayland, hidden or
minimized windows may fall back to rendering Qt widgets with GPU surfaces absent;
the response includes a warning. `screen` requests the whole primary display;
rootless Xwayland cannot provide that image and reports a uniform-image warning.
The external watchdog bounds a blocked capture together with other GUI hangs.

`--control-verbose` enables Debug-level emulator output and line flushing.
`EDEN_LOG_FILTER` selects a narrower startup filter. The launcher rotates console
logs at 8 MiB with three backups, and control logs at 4 MiB with three backups.
Control records include request IDs, method, target, outcome and operation timing;
they avoid recording arbitrary text-field values. Existing emulator logs remain
in the selected profile, with their usual size limit.

`--screenshot-interval SECONDS` keeps `latest.png` in the run directory.
`--debugger` launches Eden under gdb and records all thread backtraces on a crash
in the console log. It requires gdb and uses the same memory/runtime bounds.
Debugger runs launch the executable directly, without an intermediate shell.
Their process snapshot describes gdb; the console backtrace describes the emulator.
Full system core dumps are disabled by the bounded launcher. On termination,
`run.json`, `unit.txt` and available `/proc`/cgroup snapshots record exit status,
memory peak and watchdog reason. Diagnostic logs, screenshots and backtraces may
contain game paths or application data; store the run directory appropriately.
The manifest also retains resource samples every five seconds and the highest
observed memory peak, since systemd may remove a successful transient unit before
the final snapshot. A stop that exceeds its grace period reports `stop_timeout`;
reaching the total lifetime reports `runtime_limit`.
