param(
    [ValidateSet('Start', 'Stop', 'Status')]
    [string]$Action = 'Status',
    [string]$Unit = 'deluxe-private-display',
    [string]$Socket = 'wayland-deluxe',
    [ValidateRange(60, 43200)]
    [int]$MaxRuntimeSeconds = 3600,
    [int]$Width = 1920,
    [int]$Height = 1080
)

# Private display for emulator test runs on this host: a headless weston
# compositor with the GL renderer plus Xwayland, as a bounded user unit.
# Xwayland on it exposes DRI3/Present, so Vulkan presents on the Intel GPU
# (Xvfb does not: no DRI3, no present queue). Start it before the emulator
# unit and stop it only after the emulator has exited. Print the X display
# number weston assigned; nothing here touches the desktop session.
$ErrorActionPreference = 'Stop'

function Get-DisplayNumber {
    $lines = & journalctl --user -u "$Unit.service" --no-pager -n 200 2>$null
    $match = $lines | Select-String -Pattern 'xserver listening on display (:\d+)' | Select-Object -Last 1
    if ($match) { return $match.Matches[0].Groups[1].Value }
    return $null
}

switch ($Action) {
    'Stop' {
        & systemctl --user stop "$Unit.service"
        exit $LASTEXITCODE
    }
    'Start' {
        & systemctl --user is-active --quiet "$Unit.service"
        if ($LASTEXITCODE -eq 0) { throw "$Unit is already active. Use -Action Stop first." }
        $runtime = [Environment]::GetEnvironmentVariable('XDG_RUNTIME_DIR')
        if (-not $runtime) { $runtime = "/run/user/$(id -u)" }
        & systemd-run --user --collect "--unit=$Unit" `
            "--property=RuntimeMaxSec=$MaxRuntimeSeconds" '--property=KillMode=control-group' `
            '--property=TimeoutStopSec=5' "--setenv=XDG_RUNTIME_DIR=$runtime" `
            /usr/bin/weston --backend=headless --renderer=gl "--width=$Width" "--height=$Height" `
            "--socket=$Socket" --xwayland --idle-time=0
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        $deadline = (Get-Date).AddSeconds(20)
        $display = $null
        while (-not $display -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 500
            $display = Get-DisplayNumber
        }
        if (-not $display) { throw "weston started but no Xwayland display was announced; see journalctl --user -u $Unit" }
        [pscustomobject]@{ unit = "$Unit.service"; display = $display; wayland = $Socket; runtime_dir = $runtime } | ConvertTo-Json
    }
    default {
        & systemctl --user show "$Unit.service" --property=ActiveState,SubState,MainPID,Result
        $display = Get-DisplayNumber
        if ($display) { Write-Output "display=$display" }
    }
}
