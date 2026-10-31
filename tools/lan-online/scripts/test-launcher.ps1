$ErrorActionPreference='Stop'
$launcher=Join-Path $PSScriptRoot 'start-bounded.ps1'
$temp=Join-Path ([IO.Path]::GetTempPath()) ('nsolan-launch-probe-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $temp | Out-Null
$global:nsolanProbeCalls=0
$global:nsolanProbeState='inactive'
$global:nsolanProbeCaptured=@()
function global:systemctl { $global:LASTEXITCODE=0; $global:nsolanProbeState }
function global:systemd-run { $global:nsolanProbeCalls++; $global:nsolanProbeCaptured=@($args); $global:LASTEXITCODE=0 }
try {
 $binary=Join-Path $temp 'synthetic-executable.txt'
 [IO.File]::WriteAllText($binary,'inert launcher probe, never executed')
 $hash=(Get-FileHash -Algorithm SHA256 $binary).Hash
 $cert=Join-Path $temp 'synthetic-cert.txt'
 $key=Join-Path $temp 'synthetic-key.txt'
 [IO.File]::WriteAllText($cert,'inert'); [IO.File]::WriteAllText($key,'inert')
 foreach ($f in @('0x6e_7.bin','0x6e_9.bin','0x6e_10.bin','0x6e_11.bin','0x73_21.bin','0x73_60.bin','0x73_61.bin','0x73_84.bin','0x73_95.bin')) { [IO.File]::WriteAllBytes((Join-Path $temp $f),[byte[]]@()) }
 $config=Get-Content (Join-Path $PSScriptRoot '../config/backend.example.json') -Raw | ConvertFrom-Json -AsHashtable
 $config.NSOLAN_TLS_CERT=$cert; $config.NSOLAN_TLS_KEY=$key; $config.NSOLAN_REPLAY_DIR=$temp
 $configFile=Join-Path $temp 'config.json'
 function SaveConfig { [IO.File]::WriteAllText($configFile,($config | ConvertTo-Json)) }
 function InvokeProbe { & $launcher -Binary $binary -ExpectedSha256 $hash -Config $configFile -Unit nsolan-fixture-inert-probe -RuntimeSeconds 75 | Out-Null }
 function ExpectFailure([string]$message) {
  $before=$global:nsolanProbeCalls
  try { InvokeProbe; throw 'expected rejection absent' } catch { if ($_.Exception.Message -ne $message) { throw } }
  if ($global:nsolanProbeCalls -ne $before) { throw 'rejected configuration reached fake launch' }
 }
 SaveConfig
 InvokeProbe
 if ($global:nsolanProbeCalls -ne 1) { throw 'valid fixture did not reach fake launch' }
 foreach ($arg in @('--property=MemoryMax=512M','--property=MemorySwapMax=0','--property=RuntimeMaxSec=75','--setenv=NSOLAN_UPSTREAM_STDOUT=0','--setenv=NSOLAN_NAT_FILE=',$binary)) {
  if ($arg -notin $global:nsolanProbeCaptured) { throw 'bounded launch argument missing' }
 }
 $global:nsolanProbeState='active'; ExpectFailure 'unit already in use'; $global:nsolanProbeState='inactive'
 $config.NSOLAN_SESSION_STATIONS='raw'; SaveConfig; ExpectFailure 'release launcher requires LAN station mode'
 $config.NSOLAN_SESSION_STATIONS='lan'; $config.NSOLAN_UPSTREAM_STDOUT='1'; SaveConfig; ExpectFailure 'invalid configuration field'
 $config.Remove('NSOLAN_UPSTREAM_STDOUT'); $config.NSOLAN_TLS_CERT=Join-Path $temp 'missing'; SaveConfig; ExpectFailure 'external TLS file missing'
 $config.NSOLAN_TLS_CERT=$cert; SaveConfig; Remove-Item -LiteralPath (Join-Path $temp '0x6e_7.bin'); ExpectFailure 'reviewed initialization file missing'
 Write-Output 'PASS: one inert successful invocation; five pre-launch rejection cases; cap/stdout/NAT-file arguments verified. No systemd command or executable ran.'
} finally {
 Remove-Item Function:\systemctl,Function:\systemd-run
 Remove-Item -LiteralPath $temp -Recurse -Force
}
