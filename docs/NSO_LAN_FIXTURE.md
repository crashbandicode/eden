# Private LAN NSO fixture

This opt-in fixture supports synthetic accounts, explicit DNS mapping and private
socket routing for a separately deployed LAN companion service. It is experimental;
it does not provide Nintendo services, voice chat, or public Internet matchmaking.

## Enable and disable

Set `EDEN_NSO_LAN_CONFIG` **only in the emulator process environment** to the path
of a UTF-8 JSON file. Configuration is read once per process; restart after edits.
Keep each instance's profile, save data and synthetic account ID separate.

Example (replace these example private addresses with your own LAN addresses):

```json
{
  "account_id": 49153,
  "allowed_ips": ["127.0.0.1", "192.168.10.10", "192.168.10.11", "192.168.10.20", "192.168.10.21"],
  "dns": {
    "g23380901-lp1.s.n.srv.nintendo.net": "192.168.10.20",
    "nncs1-lp1.n.n.srv.nintendo.net": "192.168.10.20",
    "nncs2-lp1.n.n.srv.nintendo.net": "192.168.10.21",
    "api-lp1.znc.srv.nintendo.net": "192.168.10.21"
  },
  "port_redirects": [
    {"address": "192.168.10.20", "from": 443, "to": 8443},
    {"address": "192.168.10.21", "from": 443, "to": 8444}
  ]
}
```

- `account_id`: unsigned integer 1–4294967295, matching the companion's identity.
- `allowed_ips`: nonempty literal IPv4 list, limited to 10/8, 127/8,
  172.16/12 and 192.168/16. Include both peers and every companion listener.
- `dns`: exact hostname-to-allowed-address map. Names are ASCII-lowercased and
  lose one trailing dot. No wildcard or `%` keys are accepted. A guest NSD
  `-%.` template expands to `-lp1.` only if the resulting exact name is mapped.
- `port_redirects`: optional address/from/to entries; ports must be 1–65535,
  the address allowed, and each address/from pair unique.

The config must be at most 65536 bytes. Requested but missing, empty, malformed,
oversized or invalid config **fails closed**: guest DNS and address-bearing socket
connect/send/accept operations are denied; synthetic account overrides are off.
Unmapped names and unlisted destinations are denied even with valid config.
To disable, **remove the environment variable**; an empty value does not disable.
Never set it globally or point a fixture instance at an ordinary profile.

Valid config enables a synthetic account ID, async availability/license behavior
and an ephemeral RS256 ID token. Token/key material stays in memory, expires after
20 minutes and is renewed as needed. It does not authenticate with a real account.
The TLS socket ownership fix retains the frontend's shared socket reference;
TLS handshakes still execute against the actual endpoint.

## Companion deployment

The companion backend is a separate component with separately licensed code,
including a pinned Nextendo NEX core under PolyForm Shield. It is not bundled or
relicensed by this emulator change. Deployment requires its own reviewed source,
license/provenance, private TLS certificate, configured identities and listeners.
No backend binaries, replay responses, keys, profiles or game data are included here.

The tested topology uses one emulator on each of two hosts with distinct private
source IPs. NAT-check responders use two distinct addresses. The companion may use
an observed UDP endpoint only when its source IP uniquely identifies one account;
same-IP peers are unsupported. A diagnostic voice-chat endpoint returned explicit
HTTP 503 and the observed public arena proceeded without voice chat. Do not infer
voice-chat support or invent a successful API response. No global DNS/firewall
changes or real Nintendo/Nextendo accounts are required by the fixture.

### Operator procedure

1. Start the private companion using its versioned deployment instructions. It
   needs auth TLS, secure matchmaking and two NAT-check addresses. For Arena,
   start its unsupported-voice-chat diagnostic on the second address, port8444
   in the example above. Keep TLS/auth and diagnostic port mappings distinct.
   Initialization data required by that companion must be supplied separately;
   the emulator source does not include it.
2. Prepare one isolated writable Eden profile per host, with matching game,
   update and mod versions. Use account49153 on one host and49154 on the other,
   with corresponding companion identities. Configure controllers in each
   private profile. Doubles requires two connected local controllers per host.
3. Write a separate config for each host using the schema above. Include both
   peer addresses and every service address. Start each isolated Eden with the
   config variable set only for that process. In PowerShell7, for example:

   ```powershell
   $previousFixture = [Environment]::GetEnvironmentVariable('EDEN_NSO_LAN_CONFIG', 'Process')
   try {
       $env:EDEN_NSO_LAN_CONFIG = '/path/to/slot/fixture.json'
       & '/path/to/isolated-eden/eden' # Windows: the isolated eden.exe
   } finally {
       [Environment]::SetEnvironmentVariable('EDEN_NSO_LAN_CONFIG', $previousFixture, 'Process')
   }
   ```

4. Enter **Online → Smash → Quickplay → Solo** on both games. The first player
   waits until the second searches. For doubles choose **Co-op** and join both
   local controllers on each machine. For background search choose
   **Background Matchmaking**, configure it and confirm; search continues while
   browsing supported game menus. For arenas, create a public arena on one host
   and browse/join it on the other, then move both players into the ring and ready.
5. To stop, leave the session and close both isolated games before stopping the
   companion. Launch normally without the variable to return to ordinary Eden.
   This is an emulator change plus a companion service, not an Eden plugin or a
   new NRO. Existing NSO Deluxe may be tested without modifying its binary.

### Agent validation and rollout gates

Reserve separate profiles, displays and bounded run slots; preserve other
operators' processes. Record the source commit, exact emulator and companion
hashes, game/mod versions, identities, topology and run deadlines. Never print
tokens, keys or game payloads. Do not infer game input delivery from a successful
control RPC. Capture actual game screens on both peers.

Required acceptance includes first-player waiting/second-player pairing,
Quickplay gameplay/results/direct rematch, Background gameplay/results/search
continuation, Arena create/join/two completed rounds/lobby return/leave/close,
and **doubles: two local players per host, full2v2 match, matching results and
requeue into another playable match**. Repeat completed doubles matches and
capture any early exit before treating reliability as accepted. A timed-out
rematch prompt followed by a new search is not proof of selecting rematch.
Also verify the fixture-disabled path and invalid-config helper probes.

Publish only after the agreed gates pass. Stage versioned artifacts and retain
rollback versions; wait for existing Eden processes to close before changing
normal installations. Keep fixture configuration out of global environments and
preserve normal profiles. Report untested modes separately from accepted gates.

## Current acceptance limits (2026-10-16)

Real Smash Ultimate clients on Windows and Ubuntu reached synchronized Quickplay
1v1 gameplay. A public Battle Arena completed two full matches, matching results,
return to lobby, guest leave and host closure. Two local players per host in
Quickplay Co-op completed three consecutive full matches on the final release
packages, with matching team results captured on both hosts for the last two
rounds and direct rematches into subsequent gameplay. Both private profiles
require two connected controllers.

Earlier doubles attempts returned to character select early for unknown reasons;
another attempt showed an explicit inactivity disconnect after a delayed test
driver. Those failures remain part of the record. The accepted consecutive
sequence used timely controls and captured the actual rematch prompt.
Background matchmaking completed a full match with matching winner results and
continued searching into another playable match; direct timed rematch selection
remains unverified. A final-package singles match completed through sudden death
with matching winner results; selecting the timed Rematch-Yes prompt on both
clients led directly into another playable match. Both applications subsequently
exited normally through their Exit commands with exit status 0.
Earlier unsporting-behavior warnings remain recorded.
Four-player input delivery
requires real game evidence: `input.state` is lease bookkeeping and frontend
readback is not guest delivery. Arena teams, room-code joins, broader NAT layouts
and full NSO Deluxe coverage are not established. Windows and Linux release
builds of integrated commit `289c4684e7` were verified from the same frozen source;
final-package Quickplay and doubles were exercised. Arena and background
observations used the preceding private build with identical production code.
These observations satisfy the bounded runtime gates above. Normal installation
rollout and broader reliability remain separate from this acceptance.

## Standalone regression probes

These compile only fixture helpers, not the emulator. They need a C++20 compiler,
CMake, Python 3, OpenSSL Crypto and nlohmann_json development packages:

```text
cmake -S tests/nso_lan_fixture -B /path/to/probe-build
cmake --build /path/to/probe-build --parallel 1
ctest --test-dir /path/to/probe-build --output-on-failure
```

Alternatively pass built probe paths to
`python3 tests/nso_lan_fixture/run_probes.py --config-probe <exe> --token-probe <exe>`.
Config cases use fresh processes and temporary files because configuration is
immutable per process. The token probe verifies RS256 signatures with OpenSSL,
claims, expiration/renewal, distinct identities and tamper rejection without
printing tokens or keys. These probes do not test CMIF ABI, live DNS/socket
dispatch, controller connection, game delivery, or gameplay.
