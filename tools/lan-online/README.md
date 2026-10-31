# LAN Online companion source

This standalone companion supplies Eden's automatic LAN Online services and
the older two-identity NEX fixture/NNCS responder. It does not connect to Nintendo's
online service. This source snapshot matches the protocol-v4 companion used in
the October 30 validation; source publication is separate from installation.
First-party licensing is unassigned; publication grants no new license. Preserve
the independent third-party terms and notices in NOTICE.md and PROVENANCE.md.

For the current automatic mode, operator commands, platform support, persistent
ratings and acceptance limits, start with [LAN Online](../../docs/LAN_ONLINE.md).
The fixture instructions below describe the older explicit configuration mode.
Build the runtime from this directory with `go build -trimpath -buildvcs=false
-o <output>/eden-lan-runtime ./cmd/nsolan` (use `.exe` on Windows). Run the resulting
executable with `--help` for the automatic-mode arguments. The private initialization
replies remain external and are not included in Git.

## Build and test

Use Linux, PowerShell 7, Python 3.12+ and Go **1.27.2**. No toolchain or binary is
included. go.mod/go.sum pin modules and the vendored, unmodified Nextendo NEX
commit. Dependencies must be in a Go module cache or available through your
approved Go proxy. From the package directory, after host admission for a free
1GiB build slot:

```powershell
/usr/bin/pwsh -NoLogo -NoProfile -NonInteractive -Command 'systemd-run --user --wait --pipe --unit=nsolan-companion-build --property=MemoryMax=1G --property=MemorySwapMax=0 --property=RuntimeMaxSec=600 /usr/bin/pwsh -NoLogo -NoProfile -NonInteractive -File ./scripts/build-test.ps1 -Go /absolute/go/bin/go'
```

The helper verifies modules, runs vet, backend and vendored core tests, eleven
ZNC diagnostic tests, and builds ../artifacts/nsolan-companion. It uses one Go
job, CGO disabled, trimpath and buildvcs=false. Record its SHA256. Do not execute
an older bin/nsolan. Tests use private in-process loopback fixtures; the build
helper does not start a persistent backend. Build outputs must remain outside
the source package.

Run the portable launcher probe separately (it substitutes inert functions for
systemctl/systemd-run and never starts a service or executable):

```powershell
/usr/bin/pwsh -NoLogo -NoProfile -NonInteractive -File ./scripts/test-launcher.ps1
```

It checks bounded launch arguments and rejects occupied units, raw station mode,
unknown environment fields and missing external TLS/initialization inputs.

## External private inputs

Copy config/backend.example.json outside this package and replace sample IPs,
TLS paths and initialization directory. Both backend IPs must already exist on
the host; the launcher does not configure interfaces or firewalls. Match the
auth/secure ports and NNCS addresses to each Eden opt-in configuration. Use
distinct account_id values 49153/49154 and distinct client LAN addresses. LAN
station mode preserves reported UDP endpoints and falls back to in-memory NNCS
observations only when the authenticated host IP is unambiguous.

config/eden-a.example.json and eden-b.example.json are clean matching examples
for the tested game service names. Change sample client IPs 192.168.10.30/.31
and backend IPs .20/.21 together in both files and backend.example.json. Set
EDEN_NSO_LAN_CONFIG only in the private Eden launch environment to the slot's
external configuration path. Use separate profiles and two configured controllers
per profile for doubles. To disable, remove the environment variable completely;
empty or invalid values fail closed. Configuration is immutable for each process;
restart Eden after edits. Do not set global DNS, hosts or user-wide variables.
These examples target the tested title/version, not arbitrary games.

Provide your own TLS certificate/key at external paths. Neither is distributed.
Issue or renew them through your local certificate procedure with both backend
IP SANs. For an isolated self-signed test certificate, run openssl req -x509
-newkey rsa:2048 -nodes -days 30 with explicit -keyout/-out paths in a new
operator-owned directory, -subj /CN=nsolan-fixture and -addext
subjectAltName=IP:192.168.10.20,IP:192.168.10.21. Restrict that directory to 0700
and the key to 0600; never overwrite an active certificate/key. Point external
configuration at the renewed pair and restart only the owned backend after its
clients have closed. This package performs no trust-store installation.
Equivalent Quickplay startup requires separately obtained, operator-reviewed
initialization replies in NSOLAN_REPLAY_DIR:

```text
0x6e_7.bin  0x6e_9.bin  0x6e_10.bin  0x6e_11.bin
0x73_21.bin 0x73_60.bin 0x73_61.bin  0x73_84.bin 0x73_95.bin
```

No response bytes, download helper or redistribution permission are supplied.
The existing private deployment retains its own existing data. Source shape-only
fallbacks are not evidence of equivalent Quickplay initialization. The release
launcher requires all nine external files; presence checks establish neither
provenance, rights nor protocol validity. Do not put these files in this package.

## Bounded launch

After owner/admission checks, use the source-built binary and its recorded hash:

```powershell
/usr/bin/pwsh -NoLogo -NoProfile -NonInteractive -File ./scripts/start-bounded.ps1 -Binary ../artifacts/nsolan-companion -ExpectedSha256 <64-hex-build-hash> -Config /absolute/private/backend.json -Unit nsolan-fixture-private -RuntimeSeconds 1800
```

Linux systemd user service limits: 512MiB, zero swap, 1800 seconds by default
(maximum 3600). The launcher neither replaces nor stops a unit. It checks binary
identity, external inputs and allowlisted fields; the backend validates endpoints
before listening. Follow host ownership rules before launching or stopping this
exact unit. Inspect metadata with journalctl --user -u
nsolan-fixture-private.service. Never enable upstream stdout: it contains keys
and tickets. No remote actions or Eden profile mutations are performed.

## Optional unsupported ZNC diagnostic

diagnostics/znc_endpoint.py is a TLS metadata diagnostic, **not** voice-chat
support. It consumes at most 64KiB, redacts queries and unknown paths, and returns
explicit HTTP 503. For a separately admitted 128MiB slot:

```powershell
/usr/bin/pwsh -NoLogo -NoProfile -NonInteractive -Command 'systemd-run --user --unit=nsolan-fixture-znc --property=MemoryMax=128M --property=MemorySwapMax=0 --property=RuntimeMaxSec=1800 /usr/bin/python3 ./diagnostics/znc_endpoint.py --bind 192.168.10.21 --port 8444 --cert /absolute/private/tls/cert.pem --key /absolute/private/tls/key.pem'
```

The examples map api-lp1.znc.srv.nintendo.net to 192.168.10.21 and redirect that
address's guest TCP 443 to 8444. This must differ from auth's 192.168.10.20,
whose guest TCP 443 redirects to 8443; otherwise ZNC reaches the wrong listener.

## Acceptance limits

The existing private deployment demonstrated Arena two full matches/results,
return to lobby and guest leave/host close; Co-op two local players per host
completed a full match with matching team results and requeued into another
match; Background completed a full match with matching winner results and
returned through menus/search into another playable match. Direct timed
Rematch-Yes selection remains unverified. Earlier unexplained early exits remain;
these are narrow observations, not a stability claim. Source-only startup
without external initialization data has not reproduced that acceptance.
Fresh Eden main builds/installations and independent package review remain
separate coordinator gates. Packaging did not run these helpers as services.
