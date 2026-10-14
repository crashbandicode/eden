# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# MEMENTO-CTRL-MACRO: Run only in an owned, already-loaded test game. Leaves test macros as evidence.
#requires -Version 7.0
param(
    [string]$Client = 'C:/Eden/source/tools/edenctl.ps1',
    [string]$PipeName = 'eden-ally-control',
    [Parameter(Mandatory)][string]$OutputDirectory,
    [int]$Player = 0
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$prefix = 'Acceptance-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$script:checks = [Collections.Generic.List[string]]::new()
function Call([string]$Method, [hashtable]$Params = @{}, [switch]$AllowError) {
    $raw = & $Client -PipeName $PipeName -Method $Method -ParamsJson ($Params | ConvertTo-Json -Compress -Depth 20)
    $raw | Add-Content (Join-Path $OutputDirectory 'responses.jsonl')
    $reply = $raw | ConvertFrom-Json -AsHashtable
    if (!$reply.ok -and !$AllowError) { throw "$Method failed: $raw" }
    if ($AllowError) { return $reply }
    return $reply.result
}
function Check([bool]$Condition, [string]$Description) {
    if (!$Condition) { throw "Assertion failed: $Description" }
    $script:checks.Add($Description)
}
function WaitState([string]$State, [int]$Seconds = 15) {
    $until = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        $status = Call 'macro.status'
        if ($status.state -eq $State) { return $status }
        if ($status.state -eq 'error') { throw "Macro error: $($status.error)" }
        Start-Sleep -Milliseconds 40
    } while ([DateTime]::UtcNow -lt $until)
    throw "Timeout waiting for $State; last state $($status.state)"
}
function Shot([string]$Name) {
    $null = Call 'ui.screenshot' @{target='screen'; path=(Join-Path $OutputDirectory "$Name.png")}
}
$savedShortcuts = $null
try {
    $game = Call 'status'
    Check ($game.running -and $game.loading_complete) 'Test game is loaded and running'
    $savedShortcuts = Call 'macro.shortcuts'
    $null = Call 'input.reset'
    $null = Call 'macro.record' @{name="$prefix-cancelled"; player=$Player}
    $null = Call 'macro.stop'
    $cancelled = Call 'macro.inspect' @{name="$prefix-cancelled"} -AllowError
    Check (!$cancelled.ok) 'Cancelling the countdown does not create an empty macro'
    $start = Call 'macro.record' @{name=$prefix; player=$Player}
    Check ($start.state -eq 'countdown' -and $start.cue -eq 'countdown') 'Record starts in countdown'
    Shot 'countdown'
    $red = WaitState 'recording'
    Check ($red.cue -eq 'red') 'Recording has red cue'
    Shot 'recording-red'
    Start-Sleep -Milliseconds 1500
    $null = Call 'macro.marker' @{name='branch-point'}
    $null = Call 'input.stick' @{player=$Player; stick='right'; x=0.65; y=-0.25; lease_ms=500}
    Start-Sleep -Milliseconds 700
    $null = Call 'input.button' @{player=$Player; button='r'; state='tap'; duration_ms=250}
    Start-Sleep -Milliseconds 450
    $stopped = Call 'macro.stop'
    Check ($stopped.state -eq 'finished') 'Stopping recording saves it'
    $original = Call 'macro.inspect' @{name=$prefix}
    $originalPath = $original.path
    $originalHash = (Get-FileHash -LiteralPath $originalPath).Hash
    Check ($original.duration_frames -gt 30) 'Recording contains an advancing frame timeline'
    Check (@($original.events | Where-Object { [Math]::Abs($_.sticks[2] - 0.65) -lt 0.02 }).Count -gt 0) 'Mapped stick input was captured'
    Check (@($original.events | Where-Object { ($_.buttons -band 128) -ne 0 }).Count -gt 0) 'Mapped button input was captured'
    Check (@($original.markers | Where-Object name -eq 'branch-point').Count -eq 1) 'Named marker is saved'
    $again = Call 'macro.record' @{name=$prefix; player=$Player} -AllowError
    Check (!$again.ok) 'Duplicate recording name is refused'
    $play = Call 'macro.play' @{name=$prefix}
    Check ($play.state -eq 'playing') 'Named playback starts immediately'
    $busy = Call 'input.button' @{player=$Player; button='a'; state='tap'} -AllowError
    Check (!$busy.ok -and $busy.error.code -eq 'input_busy') 'CLI input cannot overwrite active playback'
    $null = Call 'game.pause'
    $pauseDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 40
        $paused = Call 'macro.status'
    } while (!$paused.paused -and [DateTime]::UtcNow -lt $pauseDeadline)
    Check ([bool]$paused.paused) 'Playback observes game pause'
    Start-Sleep -Milliseconds 300
    $stillPaused = Call 'macro.status'
    Check ($stillPaused.frame -eq $paused.frame) 'Paused playback does not advance frames'
    $null = Call 'game.resume'
    $null = WaitState 'finished'
    $null = Call 'macro.play' @{path=$originalPath}
    $null = Call 'macro.stop'
    $reset = Call 'input.reset'
    Check ($null -ne $reset) 'Input ownership is released after stop'
    $null = Call 'macro.record_from' @{name=$prefix; marker='branch-point'; new_name="$prefix-branch"}
    $blue = WaitState 'branch_playing'
    Check ($blue.cue -eq 'blue') 'Branch prefix has blue cue'
    Shot 'branch-blue'
    $tail = WaitState 'recording'
    Check ($tail.cue -eq 'red') 'Branch transitions from blue to red'
    Shot 'branch-red'
    $null = Call 'input.stick' @{player=$Player; stick='right'; x=-0.7; y=0.4; lease_ms=400}
    Start-Sleep -Milliseconds 600
    $null = Call 'macro.stop'
    $branch = Call 'macro.inspect' @{name="$prefix-branch"}
    Check ($branch.parent.name -eq $prefix -and $branch.parent.marker -eq 'branch-point') 'Branch records parent and marker'
    Check ((Get-FileHash -LiteralPath $originalPath).Hash -eq $originalHash) 'Branch leaves the original file byte-for-byte unchanged'
    $cut = @($original.markers | Where-Object name -eq 'branch-point')[0].frame
    $before = @($original.events | Where-Object { $_.frame -lt $cut }) | ConvertTo-Json -Depth 20 -Compress
    $after = @($branch.events | Where-Object { $_.frame -lt $cut }) | ConvertTo-Json -Depth 20 -Compress
    Check ($before -eq $after) 'Branch contains an identical independent prefix'
    Check (@($branch.events | Where-Object { $_.frame -ge $cut -and [Math]::Abs($_.sticks[2] + 0.7) -lt 0.02 }).Count -gt 0) 'Branch captures a different new tail'
    $null = Call 'macro.bind' @{name=$prefix; keyboard='Ctrl+Alt+9'; gamepad='l+r+plus'}
    $bound = Call 'macro.inspect' @{name=$prefix}
    Check ($bound.bindings.keyboard -eq 'Ctrl+Alt+9' -and $bound.bindings.gamepad -eq 'l+r+plus') 'Both playback shortcut types persist'
    $null = Call 'ui.key' @{target='MainWindow'; key='Ctrl+Alt+9'}
    $null = WaitState 'playing' 3
    $null = Call 'macro.stop'
    Check $true 'Keyboard playback shortcut starts the named macro'
    # Synthetic mapped pad inputs exercise chord recognition, not physical hardware.
    foreach ($button in @('l','r','plus')) {
        $null = Call 'input.button' @{player=$Player; button=$button; state='press'; lease_ms=2000}
    }
    $null = WaitState 'playing' 3
    $null = WaitState 'finished'
    Check $true 'Mapped gamepad playback shortcut starts the named macro'
    $null = Call 'macro.shortcuts' @{marker_keyboard='Ctrl+Shift+M'; marker_gamepad='l+r+minus'}
    $markerSettings = Call 'macro.shortcuts'
    Check ($markerSettings.marker_gamepad -eq 'l+r+minus') 'Both marker shortcut types are configurable'
    $null = Call 'macro.record' @{name="$prefix-shortcuts"; player=$Player; countdown_ms=0}
    $null = WaitState 'recording'
    Start-Sleep -Milliseconds 200
    $null = Call 'ui.key' @{target='MainWindow'; key='Ctrl+Shift+M'}
    Start-Sleep -Milliseconds 200
    foreach ($button in @('l','r','minus')) {
        $null = Call 'input.button' @{player=$Player; button=$button; state='press'; lease_ms=1000}
    }
    Start-Sleep -Milliseconds 250
    $null = Call 'input.reset'
    $null = Call 'macro.stop'
    $marked = Call 'macro.inspect' @{name="$prefix-shortcuts"}
    Check (@($marked.markers).Count -eq 2) 'Keyboard and mapped gamepad marker shortcuts each insert a marker'
    # Clear temporary playback bindings after activation checks.
    $null = Call 'macro.bind' @{name=$prefix; keyboard=''; gamepad=''}
    @{ok=$true; checks=$script:checks; original=$originalPath; branch=$branch.path; commit=$game.commit; physical_controller_tested=$false} |
        ConvertTo-Json -Depth 20 | Set-Content (Join-Path $OutputDirectory 'result.json')
} catch {
    @{ok=$false; checks=$script:checks; error=$_.ToString()} | ConvertTo-Json -Depth 20 |
        Set-Content (Join-Path $OutputDirectory 'result.json')
    throw
} finally {
    try { $null = Call 'game.resume' } catch {}
    try { $null = Call 'macro.stop' } catch {}
    try { $null = Call 'input.reset' } catch {}
    if ($savedShortcuts) {
        try { $null = Call 'macro.shortcuts' @{marker_keyboard=$savedShortcuts.marker_keyboard; marker_gamepad=$savedShortcuts.marker_gamepad} } catch {}
    }
}
