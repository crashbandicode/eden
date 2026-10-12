# Private display for unattended runs (Linux, Intel/Mesa)

`tools/private-display.ps1` starts a bounded headless **weston** compositor
(GL renderer) with **Xwayland** as a `systemd-run --user` unit and prints the X
display it announced. Xwayland on it exposes DRI3/Present, so Vulkan presents on
the GPU and Eden renders normally while nothing appears on the desktop. Xvfb is
not a substitute on Intel: it offers no DRI3 and no present queue.

```powershell
/usr/bin/pwsh -NoLogo -NoProfile -NonInteractive -Command "./tools/private-display.ps1 -Action Start"   # prints {"display": ":2", ...}
# launch Eden (eden-run.py) with DISPLAY=:2 WAYLAND_DISPLAY= XAUTHORITY= QT_QPA_PLATFORM=xcb
/usr/bin/pwsh -NoLogo -NoProfile -NonInteractive -Command "./tools/private-display.ps1 -Action Stop"    # only after the Eden unit has exited
```

Empty `WAYLAND_DISPLAY`/`XAUTHORITY` values matter: `eden-run.py` only
`setdefault`s those names, so an empty value keeps the desktop session's values
out. The display unit must outlive the emulator unit. The control socket and
`ui.screenshot` do not depend on the display. Verified 2026-10-12 on an Intel
Lunar Lake laptop with Mesa 26.0.8 and weston 14.0.2 (Smash at 60 FPS, 1280x720
window). Requires `weston`; `vulkan-tools` (`vkcube --wsi xcb`) is a useful
sanity check before launching Eden.