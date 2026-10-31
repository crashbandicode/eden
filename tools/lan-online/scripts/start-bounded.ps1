param(
 [Parameter(Mandatory)][string]$Binary,
 [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExpectedSha256,
 [Parameter(Mandatory)][string]$Config,
 [Parameter(Mandatory)][ValidatePattern('^nsolan-fixture-[a-z0-9-]+$')][string]$Unit,
 [ValidateRange(1,3600)][int]$RuntimeSeconds=1800
)
$ErrorActionPreference='Stop'
$Binary=(Resolve-Path -LiteralPath $Binary).Path
if ((Get-FileHash -Algorithm SHA256 $Binary).Hash -ine $ExpectedSha256) { throw 'binary identity mismatch' }
$c=Get-Content -LiteralPath $Config -Raw | ConvertFrom-Json -AsHashtable
$allowed=@('NSOLAN_ADVERTISE_HOST','NSOLAN_BIND_HOST','NSOLAN_AUTH_PORT','NSOLAN_SECURE_PORT','NSOLAN_IDENTITIES','NSOLAN_SESSION_STATIONS','NSOLAN_NATCHECK','NSOLAN_NATCHECK_ADDRS','NSOLAN_NATCHECK_PORTS','NSOLAN_TLS_CERT','NSOLAN_TLS_KEY','NSOLAN_REPLAY_DIR')
foreach ($k in $c.Keys) {
 if ($k -notin $allowed -or $c[$k] -isnot [string] -or $c[$k] -match '[\r\n]') { throw 'invalid configuration field' }
}
if ($c.NSOLAN_SESSION_STATIONS -cne 'lan') { throw 'release launcher requires LAN station mode' }
foreach ($k in @('NSOLAN_TLS_CERT','NSOLAN_TLS_KEY')) {
 if (-not [IO.Path]::IsPathFullyQualified($c[$k]) -or -not (Test-Path -LiteralPath $c[$k] -PathType Leaf)) { throw 'external TLS file missing' }
}
$replies=@('0x6e_7.bin','0x6e_9.bin','0x6e_10.bin','0x6e_11.bin','0x73_21.bin','0x73_60.bin','0x73_61.bin','0x73_84.bin','0x73_95.bin')
if (-not [IO.Path]::IsPathFullyQualified($c.NSOLAN_REPLAY_DIR)) { throw 'external reviewed initialization directory required' }
foreach ($f in $replies) {
 if (-not (Test-Path -LiteralPath (Join-Path $c.NSOLAN_REPLAY_DIR $f) -PathType Leaf)) { throw 'reviewed initialization file missing' }
}
$state=& systemctl --user show "$Unit.service" --property=ActiveState --value 2>$null
if ($state -in @('active','activating','deactivating','reloading')) { throw 'unit already in use' }
$argsRun=@('--user',"--unit=$Unit",'--property=MemoryMax=512M','--property=MemorySwapMax=0',"--property=RuntimeMaxSec=$RuntimeSeconds",'--property=KillMode=control-group','--property=UMask=0077','--property=StandardOutput=null','--property=StandardError=journal','--setenv=NSOLAN_UPSTREAM_STDOUT=0','--setenv=NSOLAN_NAT_FILE=')
foreach ($k in $c.Keys) { $argsRun += "--setenv=$k=$($c[$k])" }
& systemd-run @argsRun -- $Binary
if ($LASTEXITCODE) { throw 'bounded launch failed' }
Write-Output "Started owned $Unit.service; cap ${RuntimeSeconds}s, 512MiB, no swap."
