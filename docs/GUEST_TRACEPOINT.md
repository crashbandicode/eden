# Bounded guest tracepoint

Start Eden with its opt-in `--control-socket` endpoint. While an AArch64 game is
running, arm one instruction address. Eden captures the selected registers just
before that instruction, copies an optional small guest buffer, and continues
automatically. No client resume request is needed.

Linux example (replace the addresses with addresses from the current game run):

```text
edenctl --socket /absolute/path/eden.sock tracepoint arm 0x80001234 --register x0 --register x1 --register pc --memory-register x1 --memory-size 64 --timeout-ms 300000
edenctl --socket /absolute/path/eden.sock tracepoint status
edenctl --socket /absolute/path/eden.sock tracepoint cancel
```

Use `--memory-address 0xADDRESS` for a fixed guest buffer, or
`--memory-register x0 --memory-offset 16` for a register-relative buffer.
`--register` may be repeated; choices are `x0` through `x30`, `sp`, `pc`, and
`pstate`. Omit memory options for registers only.

For a match-length capture window, use `--timeout-ms 300000` for 5 minutes
(as above), or `--timeout-ms 600000` for 10 minutes. The tracepoint stays armed
until its first hit, cancellation, game unload, or the configured deadline;
there is no need to rearm every 30 seconds. Arming returns immediately, so the
CLI connection timeout does not need to cover the whole capture window.

Windows uses the same JSON-lines methods through the installed PowerShell client:

```powershell
& .\edenctl.ps1 -PipeName eden-control -Method tracepoint.arm -ParamsJson '{"address":"0x80001234","registers":["x0","x1","pc"],"memory_register":"x1","memory_size":64,"timeout_ms":600000}'
& .\edenctl.ps1 -PipeName eden-control -Method tracepoint.status
```

The methods are `tracepoint.arm`, `tracepoint.status`, and `tracepoint.cancel`.
`schema` describes their fields. Use strings for addresses to preserve all 64
bits. A successful arm response can already be `captured` if the game hits the
address immediately.

Each request allows exactly one hit, at most 4096 memory bytes, and a deadline
of 1–600000 milliseconds (up to 10 minutes; default 5000). Memory size defaults
to 64 bytes when a buffer is selected. Only the latest request/result is kept;
another arm is refused while one remains armed. Status is `idle`, `armed`, `captured`, `expired`,
or `cancelled`. Registers and memory appear in the captured result as hexadecimal
values. An unavailable buffer produces a memory error and no partial bytes;
the captured registers remain available.

The current backend supports x86-64 hosts and AArch64 guests with the GDB stub
disabled. The address must be four-byte aligned and mapped in the current
application process. It must actually execute before the deadline to capture.
Unloading the game cancels an armed request.
The deadline uses elapsed wall time, including time spent paused after arming.

The capture is a diagnostic observation of the hitting CPU thread. Other CPU
cores continue running, so the buffer is CPU-visible data, not a globally
atomic snapshot. GPU caches are not flushed. Guest code is not patched.

## Persistent tracing

The `trace.*` interface adds concurrent persistent points alongside the legacy
one-shot `tracepoint.*` API. Native Windows build and firmware-free end-to-end
checks cover the operations below. Release-plugin and performance acceptance
remain pending; a diagnostic plugin build cannot yet be fully replaced.
It currently targets the Qt frontend, AArch64 guests and
x86-64 Dynarmic, with GDB disabled. SDL control parity is not implemented.

Start with a configuration **before boot** to observe plugin initialization:

```text
eden --control-socket /absolute/run/control.sock --trace-config /absolute/run/trace.json game.nsp
```

Example `trace.json` (the run directory must already exist):

```json
{
  "run_directory": "/absolute/run",
  "label": "release-plugin",
  "unmapped_access": "log",
  "symbols": [{"module": "libtraining_rooms.nro", "elf": "/absolute/plugin.elf"}],
  "points": [
    {
      "target": "libtraining_rooms.nro!hook_entry",
      "registers": ["x0", "x1", "x30", "pc"],
      "captures": [{"expression": "[[x0+0x10]+8]", "type": "u32"}],
      "condition": "x1 == 3",
      "on_return": true,
      "backtrace": true,
      "rate_limit": 1000
    },
    {"target": "main+0x15d9a70", "mode": "counters"}
  ]
}
```

`release-plugin.trace.jsonl` is created exclusively: an existing file is never
truncated. Configuration errors stop launch before the guest boots. The config is
at most 1 MiB. No ELF, plugin or guest code is modified.

The same operations can be issued through the control endpoint:

```text
edenctl --socket /absolute/run/control.sock trace configure '{"run_directory":"/absolute/run","label":"release-plugin"}'
edenctl --socket /absolute/run/control.sock symbols load libtraining_rooms.nro /absolute/plugin.elf
edenctl --socket /absolute/run/control.sock modules list
edenctl --socket /absolute/run/control.sock trace arm '{"target":"libtraining_rooms.nro!hook_entry","mode":"counters"}'
edenctl --socket /absolute/run/control.sock trace counters
edenctl --socket /absolute/run/control.sock trace read '{"after":"0","limit":128}'
edenctl --socket /absolute/run/control.sock trace disarm '{"id":"1"}'
```

Configure the output before boot. Arming a counter-only point does not require a
file. `trace.arm` returns an ID; inspect `trace.list` for `pending`, `armed`,
`completed` or `disarmed`. `trace.disarm {}` disarms every point. Unloading the game
disarms all points and clears pending returns; starting another game does not
silently rearm them. `trace.clear {}` removes retained points/records while the
guest is stopped. IDs and record sequence numbers are never reused.

### Addresses and symbols

Use exactly one of `address` (absolute, four-byte aligned) and `target`
(`module+offset` or `module!symbol`). Pending module targets resolve when the
module loads, including preboot points. Loaded modules expose name, base, size
and build ID through `modules.list`. Unload/reload refreshes resolution.

NSO names are loader filenames (`main`, `rtld`, `subsdk*`). Runtime RO-service NRO
loading supplies a build ID but **no filename**. Such entries are named
`nro:<build-id>`. A supplied matching ELF can bind a requested filename alias to
that build ID. Without that identity, Eden cannot infer a stripped NRO's filename.
Guest loaders bypassing the RO service are not covered by these registration hooks.

ELF input must be ELF64 little-endian AArch64, with ordinary program/section tables.
Function/data symbols from `.symtab`/`.dynsym` are relocated relative to the lowest
PT_LOAD virtual address. Nonzero build IDs must match. For note-less, zero-based
linkle RX/R/RW layouts, Eden derives the converter's SHA-256 identity from the
page-padded load segments before NRO/MOD0 header replacement (including gaps and
optional separate BSS alignment). The combined padded image is capped at 256 MiB.
Other missing IDs do not establish binary identity. Ambiguous names/mappings
refuse resolution. Limits: 256 MiB ELF,
16 symbol sets, 250,000 symbols/file. DWARF and external debug links are unsupported.

`on_return` records LR at entry, installs a host translation trap there, and pairs
returns by process/thread/PC/entry SP. Return records always include x0. This is
normal AArch64 call/return observation, not an unwind engine: tail calls, stack
switches, nonlocal unwinds and omitted returns may leave pending entries. Pending
return storage is bounded at 1,024; exhaustion is exposed as `return_overflow`.
An older invocation with an identical point/thread/LR/SP tuple is retired as
`abandoned_returns` when a new entry reuses it, rather than reported as a second
successful return. Entry and return share a rate budget, so rate-limited records
are not guaranteed to form complete pairs.

### Captures, conditions and bounds

Expressions accept x0–x30, sp, pc, pstate, decimal/hex constants, signed constant
offsets and up to four nested `[expression]` little-endian pointer dereferences.
Raw buffers use `{"expression":"[[x0+16]+8]:16"}` or a separate `size`.
Types: `bytes`, `u8`, `u16`, `u32`, `u64`, `i32`, `f32`, `bool`, and `c-string`
with a byte `size` (also `c-string(N)`). Strings stop at NUL or the cap and report
`terminated`; nonfinite f32 values retain raw bytes and report a decode error.
Failed reads publish an error without partial captured bytes. Captures use
CPU-visible RAM, not a GPU flush or a globally atomic snapshot.

Conditions are one unsigned comparison (`==`, `!=`, `<`, `<=`, `>`, `>=`), e.g.
`x1 == 3`, `[x0+0x30] != 0`, `thread == main`. Invalid memory/overflow makes the
condition false, including `!=`. The main thread is identified at process startup.

Limits: 256 retained points; 8 captures and 4,096 total capture bytes per point;
16 backtrace addresses. `max_hits=0` means no hit-count limit; otherwise completion
occurs after that many condition-matching entries. `rate_limit` defaults to 1,000
records/second/point, maximum 10,000. Hits and condition-matching counters are never
rate limited. `mode:"counters"` avoids record construction/capture reads but still
uses the CPU trap/resume path. Near-zero overhead and the requested <5% FPS budget
are **not established**.

### Record schema and loss

JSONL and `trace.read` use schema 1. Records contain `sequence`, `point_id`,
`process_id`, `thread_id`, `core_id`, `frame` (VI vsync count), `wall_time_ns`
(Unix time), `event` (`entry`/`return`), selected `registers`, `captures`, raw
`backtrace` addresses and `locations` (module/symbol attribution retained at hit
time). All u64 fields are strings; addresses/registers use hexadecimal. A frame
number is a vsync count, not a claim that rendering or game simulation completed.

The in-memory ring retains 1,024 records. Each reader sends its last `next` as
`after`; lagging readers receive `lost` and cumulative `evicted` counts. File I/O
is outside CPU threads, draining up to 128 records every 20 ms. File overruns emit
`{"schema":1,"event":"loss","records":"N"}`. `trace.counters` separately
reports rate drops, return overflow, `file_lost`, and `output_error`. An output
write/flush failure disarms tracing. This is bounded/loss-accounted streaming, not
a guarantee that an arbitrarily hot point can be recorded losslessly.
Completed/disarmed points retain their slots until `trace.clear`. Completed game
shutdown and clear drain the bounded file backlog; socket readers can still see
an explicit loss watermark after clear, even when no records remain.

### Unmapped access and remaining capabilities

On x64 hosts, AArch64 unmapped data accesses (8/16/32/64/128-bit, including fastmem
and exclusive callbacks) report an exact instruction PC with registered module
and offset. Log mode continues with zero/discard semantics. Register/backtrace
samples in log mode remain labeled `sampled_not_fault_site`. Non-guest accesses
explicitly report unavailable context.

Set `"unmapped_access":"fault"` in the per-run `--trace-config` JSON, or call
`trace.unmapped_access` with `{"mode":"fault"}` **before boot**. This enables a
**diagnostic process stop**, not Horizon guest data-abort delivery. The backend
aborts the invalid access before result/base writeback, preserves the fault-site
registers and bounded frame-pointer backtrace, and suspends the guest process.
Fault mode requires an AArch64 guest and the x64 Dynarmic backend; other host
backends reject the option. Live mode changes return `trace_busy`.

```sh
python tools/edenctl.py --socket /path/control.sock call trace.unmapped_access '{"mode":"fault"}'
python tools/edenctl.py --socket /path/control.sock call trace.unmapped_access '{}'
```

The query returns `mode`, `run_mode`, `fault_semantics`, and decimal-string
`total`, `emitted`, `suppressed` counters. Output is bounded to the first 16
accesses and subsequent power-of-two totals; suppressed accesses still count.
When stopped, `fault` contains address, width, access kind, process/thread/core,
PC, x0–x30/SP/PC/PSTATE, `stop_requested`, `stopped`, and a description including
module/backtrace. `stopped` becomes true only after active guest execution on all
cores has exited. A latched entry guard and separate diagnostic suspension prevent
ordinary applet/process/debug resume from restarting the faulted process. New
threads inherit the diagnostic suspension; termination can still unload the run.
Use `game.stop` to unload a stopped run. The receipt survives unload; the next
boot clears counters/fault and defaults to log unless explicitly configured again.
Frame-pointer walks are bounded and may terminate on an invalid or unreadable frame.

Data watchpoints, allocation accounting, hitch sampling and interactive control
stops remain unimplemented. Backtraces are available as a per-point capture;
arbitrary-thread on-demand backtraces are not implemented. Windows uses the same
Qt methods through `edenctl.ps1 -Method trace.arm -ParamsJson '{...}'`. Start the
Windows endpoint with an absolute pipe name, for example
`--control-socket \\.\pipe\eden-control`; pass `-PipeName eden-control` to the client.
Fifty-point/ten-minute release-plugin
acceptance, real load-order changes, known-allocation attribution, matched counter
comparison and performance measurements remain pending.

### Native Windows validation

Validated on an AMD Z1 Extreme Windows host with MSVC and Qt 6.8.3. The build used
12 parallel jobs, all 16 logical CPUs, and a 5.5 GiB aggregate Job Object limit.

- Five tracing test executables passed in the full build tree.
- Dynarmic's `[tracepoint]` cases passed 125 assertions across two test cases.
- The real Qt frontend booted an original firmware-free AArch64 NRO with a
  matching ELF and build ID, using the Null renderer. Fifty preboot points
  resolved by symbol/module offset. Forty-eight counters each reached exactly
  100 hits; 32 typed-capture records and 16 entry/return pairs matched independent
  guest register/memory/return-value oracles.
- The named-pipe API and JSONL output agreed on 64 records. Disarm stopped counts;
  stop/clear/reboot did not rearm old points; the frontend exited successfully.
- Boolean settings were exercised through `settings.set` in both directions.

These checks cover the real control/loader/JIT/capture/output/lifecycle path.
They do not establish Skyline loader coverage, ten-minute gameplay stability,
lossless capture of arbitrarily hot points, or an FPS overhead budget.
