# Record a GIF

While a game is running, choose **Tools → Record GIF…**, choose a new `.gif`
filename, and play. Choose **Stop GIF Recording** in the same menu to save early.
The GUI records up to 30 seconds at 10 fps and 640 pixels wide. Recording also
finishes when the game stops or Eden closes normally.

GIFs contain the rendered game image, without desktop windows or emulator menus.
They loop indefinitely and have no audio. GIF's 256-color palette can produce
color banding. Capture adds GPU readback work; reduce the size or frame rate if
the game slows down. Keep the game window visible; a hidden/minimized Vulkan window
may not produce frames. Frames are skipped when capture/encoding cannot keep up;
their timestamps preserve playback speed. Memory use is bounded independently
of recording duration. Paused time appears as a held frame and counts toward
the duration limit.

Existing output files are refused. The output directory must exist. A file
becomes available only after successful finalization; an empty recording or
write failure does not publish an incomplete GIF. A forced process termination
cannot finalize a recording.

## CLI

Start Eden with its existing opt-in `--control-socket` option, then use:

```text
python tools/edenctl.py --socket /absolute/eden.sock recording start /absolute/clip.gif --fps 15 --width 640 --duration-ms 10000
python tools/edenctl.py --socket /absolute/eden.sock recording status
python tools/edenctl.py --socket /absolute/eden.sock recording stop
```

The path is on the **Eden host**. The equivalent JSON-lines methods work through
the Windows control named pipe as well:

```json
{"id":1,"method":"recording.start","params":{"path":"C:/Eden/clips/match.gif","fps":15,"width":640,"duration_ms":10000}}
{"id":2,"method":"recording.status","params":{}}
{"id":3,"method":"recording.stop","params":{}}
```

On Windows, PowerShell 7's client sends those methods directly. For Eden launched
with `--control-socket \\.\pipe\eden-control`:

```powershell
./tools/edenctl.ps1 -Method recording.start -ParamsJson '{"path":"C:/Eden/clips/match.gif","duration_ms":10000}'
./tools/edenctl.ps1 -Method recording.status
./tools/edenctl.ps1 -Method recording.stop
```

Use `-PipeName` if you chose a different pipe name.

`start` accepts integer `fps` (1–30, default 10), `width` (160–1280, default 640),
and `duration_ms` (100–120000, default 30000). The height follows the game's
configured aspect ratio. Start requires a running game that has finished loading.
Only one recording can run at once. `stop` is idempotent.

Status reports `state` (`idle`, `recording`, `finished`, or `error`), `path`,
`frames`, `dropped_frames`, `elapsed_ms`, `duration_ms`, `fps`, `width`, `height`,
and `error`. Check `state` and `error` after stopping or reaching the limit;
successful acceptance of a start command does not guarantee that later disk
writes succeed. GUI and CLI controls operate the same recording.
