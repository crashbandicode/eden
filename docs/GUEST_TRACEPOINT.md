# Bounded guest tracepoint

Start Eden with its opt-in `--control-socket` endpoint. While an AArch64 game is
running, arm one instruction address. Eden captures the selected registers just
before that instruction, copies an optional small guest buffer, and continues
automatically. No client resume request is needed.

Linux example (replace the addresses with addresses from the current game run):

```text
edenctl --socket /absolute/path/eden.sock tracepoint arm 0x80001234 --register x0 --register x1 --register pc --memory-register x1 --memory-size 64 --timeout-ms 5000
edenctl --socket /absolute/path/eden.sock tracepoint status
edenctl --socket /absolute/path/eden.sock tracepoint cancel
```

Use `--memory-address 0xADDRESS` for a fixed guest buffer, or
`--memory-register x0 --memory-offset 16` for a register-relative buffer.
`--register` may be repeated; choices are `x0` through `x30`, `sp`, `pc`, and
`pstate`. Omit memory options for registers only.

Windows uses the same JSON-lines methods through the installed PowerShell client:

```powershell
& .\edenctl.ps1 -PipeName eden-control -Method tracepoint.arm -ParamsJson '{"address":"0x80001234","registers":["x0","x1","pc"],"memory_register":"x1","memory_size":64,"timeout_ms":5000}'
& .\edenctl.ps1 -PipeName eden-control -Method tracepoint.status
```

The methods are `tracepoint.arm`, `tracepoint.status`, and `tracepoint.cancel`.
`schema` describes their fields. Use strings for addresses to preserve all 64
bits. A successful arm response can already be `captured` if the game hits the
address immediately.

Each request allows exactly one hit, at most 4096 memory bytes, and a deadline
of 1–30000 milliseconds (default 5000). Memory size defaults to 64 bytes when a
buffer is selected. Only the latest request/result is kept; another arm is
refused while one remains armed. Status is `idle`, `armed`, `captured`, `expired`,
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
