# NX2 startup and programmatic control

The NS2 integration imports experimental Switch 2 support. Launch configuration,
the existing Qt control API and `edenctl` provide programmatic access without
desktop input injection. Startup environment controls are read at process startup
or first use; changing a shell variable after Eden starts does not reconfigure it.

This page describes source behavior imported from release `b7ff03036c` and the
bounded launcher's environment handling. It does not establish game compatibility
or Linux gameplay results. `NX2_NOTES.md` is a chronological development log;
later entries and current source supersede its older status sections.

## Boot a loose ExeFS

Use the `main` file, with `main.npdm`, `rtld` and the title's other modules beside
it. The loader recognizes `main` as a whole deconstructed ExeFS rather than
loading it as an isolated NSO. It uses the first `.romfs` file in that directory,
otherwise a `romfs/` directory beside the `exefs/` directory:

```text
title/
  exefs/
    main
    main.npdm
    rtld
    ...
  romfs/
    ...
```

With the Qt frontend and its control endpoint running, use an absolute file path:

```powershell
$env:EDEN_CONTROL_SOCKET='/tmp/nx2-run-1/control.sock'
/usr/bin/python3 tools/edenctl.py --wait 30 call game.load '{"path":"/absolute/title/exefs/main"}'
/usr/bin/python3 tools/edenctl.py status
/usr/bin/python3 tools/edenctl.py call game.pause
/usr/bin/python3 tools/edenctl.py call game.resume
/usr/bin/python3 tools/edenctl.py --wait 10 call game.stop
```

Load/stop operations can open profile, error or confirmation dialogs. Inspect
`tree`, operate the discovered dialog controls and inspect `jobs.get` when needed;
a client wait timeout does not cancel an accepted operation. See
[CLI control](CLIControl.md) for the complete contract.

The SDL frontend also accepts a direct startup command:

```powershell
eden-cli -c '/absolute/sdl2-config.ini' -g '/absolute/title/exefs/main'
```

`eden-cli` has no `--control-socket` registration and cannot be attached through
`edenctl`. The bounded `tools/eden-run.py` launcher below is **Qt-only**: it appends
Qt control arguments and supervises that endpoint. Do not pass `eden-cli` to it.

Sources: `src/core/loader/loader.cpp:GetLoader`,
`src/core/loader/deconstructed_rom_directory.cpp:FindLooseRomFS` / `Load`,
`src/core/launch_params.cpp` and `src/yuzu_cmd/yuzu.cpp`.

## Memory and core settings

The NPDM parser detects the Switch 2 ACID layout. When booting a detected loose
ExeFS, `System::Impl::Load` selects the **12 GiB emulated memory layout** as a
per-game value before kernel initialization. Global setting restoration can bring
back the configured layout after the game stops. The imported build has six
compiled CPU cores; there is no user-selectable core-count setting.

The existing generic API exposes settings, including their effective value,
canonical enum name, range and `runtime_modifiable` metadata:

```powershell
/usr/bin/python3 tools/edenctl.py settings
/usr/bin/python3 tools/edenctl.py call settings.get '{"key":"core.memory_layout_mode"}'
/usr/bin/python3 tools/edenctl.py call settings.set '{"key":"core.memory_layout_mode","value":4}'
/usr/bin/python3 tools/edenctl.py call settings.set '{"key":"core.use_multi_core","value":true}'
```

Set memory/multicore options **before loading**. They are not runtime-modifiable;
the API rejects changes while emulation is powered on. Memory enum values 0–4
mean 4/6/8/10/12 GiB. The automatic NX2 selection can override a smaller requested
layout. `settings.save` persists settings when desired; it is not required to
apply a value for the current process. Other renderer/ASTC/accuracy options use
the same generic interface, not new NX2-specific setters.

For direct SDL startup, the corresponding INI entries are:

```ini
[Core]
memory_layout_mode\default=false
memory_layout_mode=4
use_multi_core\default=false
use_multi_core=true
```

The `\default=false` companions are important: the generic config reader can
otherwise replace a manually entered value with its default. The launcher's host
cgroup memory cap is a separate limit from emulated DRAM; its default 8 GiB cap
is not a promise that a 12 GiB-layout workload fits.

Sources: `src/core/file_sys/program_metadata.cpp`, `src/core/core.cpp`,
`src/core/hardware_properties.h`, `src/common/settings.h`,
`src/common/settings_enums.h`, `src/frontend_common/config.cpp` and
`src/yuzu/control_adapter.cpp`.

## Pass startup controls through the bounded Qt launcher

Set variables in the launching process before starting Eden. This example enables
GPU-zone profiling and explicitly selects the unsplit GPU path:

```powershell
$env:EDEN_NX2_GPU_PROFILE='1'
$env:EDEN_NX2_SPLIT='0'
/usr/bin/python3 tools/eden-run.py --background --memory-gib 24 --runtime 600 --output /tmp/nx2-run-1 -- /absolute/path/to/eden
```

Choose a host memory budget appropriate to the machine; 24 GiB here is an example
cap, not a measured requirement. Use a new run directory. While the supervisor
is running, issue `edenctl` commands from another terminal. Its runtime, memory,
responsiveness and optional stall bounds remain those documented in
[CLI control](CLIControl.md).

The launcher explicitly forwards only the imported names in
[`tools/nx2_environment.py`](../../tools/nx2_environment.py), alongside its existing
display/XDG/Vulkan/log allowlist. Each supplied value is passed as one literal
`--setenv=NAME=VALUE` argument; values are not interpreted as shell commands.
Unknown `EDEN_NX2_*` names and unrelated caller variables are not exported by this
mechanism. There is no wildcard export. The obsolete comment names
`EDEN_NX2_TC_` and `EDEN_NX2_RT_TRACE_AFTER_UPSCALE` are not controls; the latter's
actual consumer reads `EDEN_NX2_TRACE_AFTER_UPSCALE`.

Known NX2 variables **absent from the caller** are added to the transient service's
`UnsetEnvironment` property. This final systemd environment-removal step prevents
stale values in the user manager from silently enabling those controls. Explicitly
supplied names are excluded from that removal list. This does not reset unrelated
manager environment variables or modify the manager's persistent environment.

`run.json` contains `nx2_environment`, a map of the supplied allowlisted controls
and their exact values. It is empty when none were supplied. It records launch
intent, not confirmation that an output file was produced or that a gated feature
became active. These controls are nonsecret diagnostic/configuration values; the
manifest does not snapshot the caller's entire environment.

To disable a **presence-based** switch, remove it rather than assigning `0`:

```powershell
[Environment]::SetEnvironmentVariable('EDEN_NX2_GPU_PROFILE', $null, 'Process')
```

A supplied empty string remains present and is forwarded unchanged. Direct
`eden`/`eden-cli` launches inherit their parent's environment normally; the
bounded launcher's manager-isolation policy applies only to its transient unit.

## Common controls and their actual meanings

All names below start with `EDEN_NX2_`. Values are forwarded unchanged; the
emulator's consumer defines parsing and platform support. Restart the process to
change cached controls.

| Control | Meaning / default |
|---|---|
| `SPLIT=0/1/2` | Requested GPU-thread mode: off, engine methods on the back thread, or macros on the front too. Default 2. Actual activation requires NX2/Ampere mode, Vulkan and compatible GPU/DMA accuracy; front macros additionally require x86-64 and enabled MME JIT. A request is not proof the split is active. |
| `MME_JIT=0`, `MME_MEMO=0` | Disable the respective native macro optimization; unset uses the default enabled path. |
| `SPLIT_CHECK`, `SPLIT_VERIFY`, `MME_MEMO_STATS` | Presence enables developer ownership/register verification or memo counters. `0` is still present. |
| `GPU_PROFILE` | Presence enables GPU-thread exclusive zone timers/counters, logged every 10 seconds. Common renderer code, not the Windows host sampler. |
| `IPC_TRACE`, `IPC_SUMMARY`, `MEM_TRACE`, `AUDIO_LEVEL` | Diagnostic IPC, memory or audio observations. These presence-based probes are normally off when unset. |
| `SVC_TRACE=1` or `SVC_TRACE=<first>:<last>` | Trace all SVCs, or only those within an inclusive NX2 present-count range. Unset is off; a present value without a colon, including `0`, traces all presents. |
| `AUTO_STUB` | Presence allows unknown-command success fallback independently of config. `=0` does not disable it. Switch 2 callers and `nx2:` services already have independent fallback rules; `core.use_auto_stub=false` is not an absolute NX2-stubbing off switch. Success does not imply the command is implemented. |
| `FRAMES_AHEAD=0` | Disable previous-frame waiting and wait on the current frame instead. Default is enabled in the imported source; older NX2_NOTES entries describe earlier failures. |
| `VSYNC_HZ=<integer>` | Experimental rate for the service's vsync event; default 60 Hz. This is not a guarantee of achieved FPS. |
| `FRAME_CAPTURE=<existing directory>` | Renderer TGA capture at presents 1, 121, 241, …, using a 1280×720 output layout. It is not the Qt window screenshot command. |
| `DUMP=<present>:<hex address>:<hex size>[,...]` | One present-selected GPU range dump into FRAME_CAPTURE's directory, or `.` if no directory was supplied. |
| `VDEC_DUMP=<existing directory>` | Quarter-resolution PGM samples from every 120th decoded video frame. Separate from presented-frame capture. |
| `SHADER_DUMP=<directory>`, `TEXTURE_DUMP=<path>`, `MME_RECORD=<file>` | Developer artifact outputs; consult the corresponding source consumer for selection/format. They do not create general-purpose managed recording jobs. |
| `TENSOR` | Unset rejects the costly HMMA lowering for the fallback path; `stub` makes matrix output D=C; other present values enable full lowering. `TENSOR=0` is **not off**. Default upscale may use a bilinear stand-in rather than full tensor emulation. |
| `ASTC_BC1=0`, `FENCE_BATCH=0`, `CNTPCT_PAUSE=0`, `INLINE_DEFER=0`, `SKIP_SAME=0` | Disable the corresponding imported optimization. These use value checks, unlike presence-based probes. |
| `BUFFER_DOWNLOADS=0/1` | Explicitly override the imported buffer-download behavior. |
| `TRANSCODE_THREADS=<count>` | Override the ASTC worker count (minimum 1). Unset defaults to a quarter of host hardware threads, at least 2. |
| `TC_MIN_AGE=<frames>`, `TC_EXPECTED_MIB=<MiB>`, `TC_CRITICAL_MIB=<MiB>` | Experimental texture-collector retention/threshold overrides. |
| `MACRO_COPY` | Presence forces the copy path, even with value `0`; unset retains the imported fast path. |

The allowlist also retains low-level experiments: `3D_METHODS`, `ASTC_FLUSH`,
`CB24_TRACE`, `FORCE_LOD0`, `IDLE_SPIN_US`, `METHOD_TRACE`, `MME_DUMP`, `MME_LOG`,
`MME_RECORD_SKIP`, `PARK_SPLIT_METHODS`, `PB_CHECK`, `PB_DUMP`, `PIPELINE_DIFF`,
`PULLER_TRACE`, `QMD_TRACE`, `QUERY_TRACE`, `REJECT`, `RT_TRACE`, `SAMPLE_AT`,
`SEM_WATCH`, `SEM_WATCH_EMITS`, `SKIP_VS`, `SPLIT_SKIP`, `TRACE_AFTER_UPSCALE`,
`TRACE_DST`, `TRACE_PS`, `TRACE_VS`, `UPSCALE_SOURCE`, `UPSCALE_SOURCE_FORMAT`,
`WINDOW_TRACE` and `WINDOW_TRACE_SKIP`. These are source-level probes, not stable
cross-version diagnostic formats. Their consumers are in GPU engines,
`dma_pusher.cpp`, Vulkan rasterizer/pipeline/texture code, `shader_environment.cpp`,
`texture_cache/texture_cache.h` and `core/hle/kernel/physical_core.cpp`.

Other main sources: `src/video_core/gpu_split.cpp:GpuSplit::MaybeStart`,
`src/video_core/nx2_profile.h`, `src/core/hle/service/service.cpp`,
`src/core/hle/service/nx2_gsv.h`,
`src/shader_recompiler/frontend/ampere/lower_to_maxwell.h`,
`src/video_core/textures/workers.cpp` and
`src/video_core/engines/maxwell_3d.cpp`.

## Frontend/platform boundaries and stopping

- Qt already supports `input.*`, `macro.*`, `ui.screenshot` and
  `recording.start/stop/status`; discover `schema` and use `edenctl call` for
  registered methods. See [CLI control](CLIControl.md) and
  [GIF recording](GIFRecording.md).
- SDL-only `HIDE_WINDOW` adds hidden/not-focusable flags when its value does not
  begin with `0`. It still creates an SDL window; it is not display-free rendering.
  Qt uses `--control-background` instead.
- SDL-only `KEY_SCRIPT` accepts ordered `seconds:scancode` or
  `f<present>:scancode` entries, separated by commas, or `@<file>`. Negative codes
  release the SDL keyboard mapping. `KEY_RECORD=<file>` records SDL key events
  with present indices. These files are not Qt controller macro files; their
  timing clocks differ. There is no attachable SDL script status/cancel API.
- `SAMPLE=<comma-separated thread names>` and its `SAMPLE_START` (60 seconds),
  `SAMPLE_SECONDS` (30 seconds), `SAMPLE_OUT` (`nx2_samples.txt`, plus `.folded`)
  companions are **Windows SDL-only** host sampling controls. A trailing `*`
  matches a thread-name prefix. The non-Windows implementation is a no-op and Qt
  does not start this sampler. `CRASH_DUMP=<directory>` is likewise registered
  only by the Windows SDL frontend and writes a dump on an unhandled exception.
  Forwarding these variables to Linux Qt does not add the missing functionality.
- Core frame/video/memory dumps are startup diagnostics. Their environment hooks
  do not provide a general managed start/stop/status protocol or reliable I/O
  error acknowledgment. Inspect the artifacts and logs; `run.json` alone is not
  completion evidence. Stop the game through `game.stop` where supported, or end
  the bounded Qt run through its supervisor/runtime limit. SDL has its normal
  process/window lifetime; there is no promised SDL `edenctl` lifecycle parity.
- Task/mesh shader support and guessed/stubbed services are emulation limitations,
  not solved by enabling a launch variable. Imported experimental controls and
  this documentation do not establish full Switch 2 emulation on any platform.

SDL sources: `src/yuzu_cmd/yuzu.cpp:SDL_AppInit` / `RunKeyScript`,
`src/yuzu_cmd/emu_window/emu_window_sdl3.{h,cpp}` and
`src/yuzu_cmd/nx2_sampler.cpp:StartFromEnvironment`.
