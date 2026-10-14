# Input macros

Open **Tools → Input Macros…** to record and replay a named controller sequence.

New to macros? Follow the [short GIF walkthrough: reuse a route with branches](InputMacrosTutorial.md).

1. Put the game at the starting menu or scene you want to reuse. Choose the player
   and enter a new recording name.
2. Click **Record new**. The **3–2–1** countdown gives you time to return to the game;
   **red** means your mapped controller inputs are being recorded.
3. Insert markers at useful branch points using the configured keyboard shortcut
   or gamepad combination. Both kinds of shortcut are supported. The default
   keyboard marker shortcut is **Ctrl+Shift+M**.
4. Stop to save the named file. **Ctrl+Shift+F12** stops the current operation.
5. Select the saved macro and assign its playback keyboard shortcut, gamepad
   combination, or both. Playback starts immediately when invoked.

Choose marker combinations that are unused in the game. Their button presses
still reach the game and are part of the recorded inputs.

## Branch from a marker

Select a saved macro and marker, enter a new name, and choose **Record from marker**.
Return the game to the same starting state first. After the countdown, **blue**
means the existing prefix is playing. At the marker the indicator switches to
**red** and your new inputs are recorded. Stop to save the branch.

The branch contains its own copy of the prefix and new tail. The original file
is preserved; moving or deleting it does not break the new branch.

## Files and timing

The library lives in `input_macros` under Eden's configuration directory. Files
use the `.edenmacro.json` extension and contain the name, game ID, player, input
timeline, markers, shortcut bindings, and optional parent information. You can
also open an explicit file outside the library.

Files recorded to a custom path, or given a playback binding, are remembered in
the library without copying them. Both name-based CLI playback and shortcuts
continue to work after restarting Eden. The external-file list is limited to
256 entries.

Timing uses displayed emulation frames, not elapsed wall-clock time. A paused
game does not advance the macro. Inputs shorter than one displayed frame are
not guaranteed to be captured. These files replay mapped inputs; they do not
restore a savestate. Use the same game version, controller configuration, and
starting scene. Random game behavior, changed menus, loading times, and game
performance can change the outcome of a replay.

Recording automatically saves at 60,000 changed input samples or 216,000 frames,
whichever comes first. These limits bound capture and save-time memory. Turbo
and motion are sampled at display-frame resolution; subframe timing is not preserved.
Keyboard and gamepad shortcuts require Eden to be the active application.

Playback takes ownership of controller input until it finishes or is stopped.
Concurrent CLI input mutations are rejected with `input_busy`; use `macro.stop`
before taking manual CLI control again.

## CLI

Eden must have been started with its opt-in `--control-socket` interface.
On Linux:

```text
edenctl --socket /tmp/eden.sock macro record "Training route"
edenctl --socket /tmp/eden.sock macro marker --name "Character select"
edenctl --socket /tmp/eden.sock macro stop
edenctl --socket /tmp/eden.sock macro play --name "Training route"
edenctl --socket /tmp/eden.sock macro record-from --name "Training route" --marker "Character select" --new-name "Mario route"
edenctl --socket /tmp/eden.sock macro bind --name "Mario route" --keyboard "Ctrl+Alt+1" --gamepad "l+r+plus"
edenctl --socket /tmp/eden.sock macro shortcuts --marker-keyboard "Ctrl+Shift+M" --marker-gamepad "l+r+minus"
edenctl --socket /tmp/eden.sock macro status
edenctl --socket /tmp/eden.sock macro list
```

Use `--path /absolute/path/file.edenmacro.json` instead of `--name` to play or
inspect an explicit file. Paths are interpreted on the machine running Eden.

Windows PowerShell 7 uses the same API through the named-pipe client:

```powershell
./tools/edenctl.ps1 -PipeName eden-ally-control -Method macro.play -ParamsJson '{"name":"Training route"}'
./tools/edenctl.ps1 -PipeName eden-ally-control -Method macro.status
./tools/edenctl.ps1 -PipeName eden-ally-control -Method macro.stop
```

Agents can call `macro.play` with `{"name":"Training route"}` or
`{"path":"C:/macros/route.edenmacro.json"}` directly. It starts playback in the
current game immediately, without a countdown or a queued game-loading job.
`macro.status` reports progress and `macro.stop` releases playback inputs.
See `schema` for all `macro.*` parameters.
