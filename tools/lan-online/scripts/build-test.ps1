param(
 [string]$Go = 'go',
 [string]$Python = '/usr/bin/python3',
 [string]$OutputDirectory = (Join-Path $PSScriptRoot '../../artifacts')
)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$env:GOTOOLCHAIN='local'
$env:GOFLAGS='-p=1 -mod=readonly'
$env:GOMAXPROCS='1'
$env:GOMEMLIMIT='768MiB'
$env:CGO_ENABLED='0'
$env:GOCACHE=Join-Path $OutputDirectory 'go-cache'
$env:GOTMPDIR=Join-Path $OutputDirectory 'go-temp'
New-Item -ItemType Directory -Force $env:GOTMPDIR | Out-Null
$version=& $Go version
if ($LASTEXITCODE -ne 0 -or $version -notmatch '^go version go1\.27\.2 ') { throw 'Go 1.27.2 required' }
Push-Location $root
try {
 & $Go mod verify
 if ($LASTEXITCODE) { throw 'module verification failed' }
 & $Go vet ./...
 if ($LASTEXITCODE) { throw 'vet failed' }
 & $Go test -count=1 ./...
 if ($LASTEXITCODE) { throw 'backend tests failed' }
 Push-Location third_party/nextendo-nex
 try {
  & $Go test -count=1 .
  if ($LASTEXITCODE) { throw 'vendored core tests failed' }
 } finally { Pop-Location }
 Push-Location diagnostics
 try {
  $env:PYTHONDONTWRITEBYTECODE='1'
  & $Python -m unittest -v test_znc_endpoint
  if ($LASTEXITCODE) { throw 'diagnostic tests failed' }
 } finally { Pop-Location }
 $exe=Join-Path $OutputDirectory 'nsolan-companion'
 & $Go build -trimpath -buildvcs=false -o $exe ./cmd/nsolan
 if ($LASTEXITCODE) { throw 'build failed' }
 Get-FileHash -Algorithm SHA256 $exe
} finally { Pop-Location }
