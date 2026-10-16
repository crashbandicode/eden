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
    "nncs2-lp1.n.n.srv.nintendo.net": "192.168.10.21"
  },
  "port_redirects": [{"address": "192.168.10.20", "from": 443, "to": 8443}]
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

## Current acceptance limits (2026-10-16)

Real Smash Ultimate clients on Windows and Ubuntu reached synchronized Quickplay
1v1 gameplay. A public Battle Arena completed two full matches, matching results,
return to lobby, guest leave and host closure. Two local players per host in
Quickplay Co-op completed one full match with matching team results and requeued
into another match. Both private profiles require two connected controllers.

Other doubles attempts returned to character select early for unknown reasons;
stable repeated full matches and a dedicated rematch prompt are not established.
Background matchmaking reached gameplay but an unsporting-behavior warning was
observed; its results/rematch gate remains pending. Four-player input delivery
requires real game evidence: `input.state` is lease bookkeeping and frontend
readback is not guest delivery. Arena teams, room-code joins, broader NAT layouts
and full NSO Deluxe coverage are not established. Canonical merge builds and normal
installation acceptance are separate work; the earlier task-private binaries do
not validate a newly integrated commit.

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
