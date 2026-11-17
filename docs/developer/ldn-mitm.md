# ldn_mitm-compatible Local Wireless

The optional `ldn_mitm` backend connects the game's Local Wireless service to compatible peers on the selected real IPv4 LAN interface. The default backend remains `room`. This is separate from LAN Online, which serves supported online game modes through its companion. Selecting this backend does not modify LAN Online or airplane-mode settings.

## Configure and control

Start Eden with its opt-in [control endpoint](../user/CLIControl.md). Before loading a game, explicitly disable airplane mode and LAN Online in the test profile and select a real interface. These examples use the Python client on Unix; replace `enp1s0` with your interface name:

```powershell
/usr/bin/python3 tools/edenctl.py --socket /tmp/eden-control.sock call settings.set '{"key":"core.airplane_mode","value":false}'
/usr/bin/python3 tools/edenctl.py --socket /tmp/eden-control.sock call settings.set '{"key":"core.lan_online","value":false}'
/usr/bin/python3 tools/edenctl.py --socket /tmp/eden-control.sock ldn configure ldn_mitm --interface enp1s0
/usr/bin/python3 tools/edenctl.py --socket /tmp/eden-control.sock call settings.save
/usr/bin/python3 tools/edenctl.py --socket /tmp/eden-control.sock ldn start
/usr/bin/python3 tools/edenctl.py --socket /tmp/eden-control.sock ldn status
/usr/bin/python3 tools/edenctl.py --socket /tmp/eden-control.sock ldn stop
```

`EDEN_CONTROL_SOCKET` can supply the socket path. Windows uses the same JSON methods through the named-pipe client:

```powershell
& tools/edenctl.ps1 -PipeName eden-control -Method ldn.configure -ParamsJson '{"backend":"ldn_mitm","interface":"Ethernet"}'
& tools/edenctl.ps1 -PipeName eden-control -Method ldn.start
& tools/edenctl.ps1 -PipeName eden-control -Method ldn.status
& tools/edenctl.ps1 -PipeName eden-control -Method ldn.stop
```

The GUI's **Network > Local Wireless Backend** selector stores the same `core.ldn_mitm` setting. **Network Interface** stores `core.network_interface`. Both selectors are locked while emulation is powered on, matching `settings.set` and `ldn.configure`. Changing either selection while powered off stops previous MITM listeners before applying the change. The MITM selector leaves the airplane-mode and LAN Online checkboxes as selected.

`core.ldn_mitm` defaults to `false`, is persistent, and is discoverable with `settings.list` and `settings.get`. `settings.set` accepts a JSON boolean. `ldn.configure` applies backend/interface together after validating the full parameter object. It changes in-memory settings; `settings.save` persists them. Omitting `interface` retains the existing selection. `ldn.start` resolves that selection to a real IPv4 interface and returns an actionable error if it is unusable.

| Method | Parameters | Behavior |
| --- | --- | --- |
| `ldn.configure` | `{"backend":"room"\|"ldn_mitm","interface":"optional name"}` | Select before boot; no change is applied for invalid parameters. |
| `ldn.start` | `{}` | Bind the selected MITM listeners without a game or frontend room. Repeated start is idempotent. Guest LDN initialization also starts the selected backend. |
| `ldn.stop` | `{}` | Close owned MITM listeners and peers. Repeated stop is idempotent. Eden and LAN Online continue running. |
| `ldn.status` | `{}` | Inspect configuration and actual runtime state. |

`ldn.status` returns `configured_backend`, `active_backend`, `running`, `configured_interface`, `interface`, `ip`, `port`, `role`, `peers`, `pending_commands`, `counters`, `last_error`, `airplane_mode`, and `lan_online`. `running` comes from the runtime's listeners. A selected but stopped MITM backend reports `active_backend:"none"`; the default selection reports `room`. Interface/IP describe the runtime's latest configuration, separately from the selected interface. Counters `received`, `sent`, and `malformed` are decimal strings to preserve integer precision. `last_error` is the runtime's last error; interface-resolution errors also appear directly in failed `ldn.start` responses.

Control errors preserve the usual `{ok:false,error:{code,message}}` response. In particular, `not_runtime_modifiable` retains the existing powered-on setting lock, `ldn_not_selected` identifies a start request with the room backend selected, `invalid_configuration` reports interface resolution, and `ldn_start_failed` reports a listener/start failure. Python CLI success exits 0, server errors exit 1, and client/argument errors exit 2. `schema` lists methods and parameters.

To return to room mode after stopping emulation:

```powershell
/usr/bin/python3 tools/edenctl.py --socket /tmp/eden-control.sock ldn configure room
/usr/bin/python3 tools/edenctl.py --socket /tmp/eden-control.sock call settings.save
```

## LAN and compatibility scope

Discovery uses IPv4 UDP port **11452** and sessions use TCP port **11452**. Peers need direct LAN connectivity and broadcast discovery on the selected interface. WAN access, Internet DNS, a relay, and the LAN Online companion are unnecessary for this protocol. A Switch still needs to remain associated with the LAN when WAN access is blocked.

Existing LAN Online destination and TCP-listener restrictions remain active if its automatic transport or a restricted fixture was separately requested. Use the explicit settings above for Local Wireless tests; combined operation is not a validated configuration. Selecting this backend does not disable those restrictions.

Switch-side ldn_mitm is a separate sysmodule installation. Firmware/Atmosphere compatibility, Switch installation, Eden-to-Eden sessions, Switch interoperability, and gameplay/desync acceptance need their own validation. This document makes no hardware compatibility or gameplay acceptance claim.

## Wire format and provenance

The 12-byte little-endian header contains magic `0x11451400`, type, compression flag, wire body length, decoded length, and two reserved bytes. Types Scan, ScanResp, Connect, and SyncNetwork have decoded payload sizes 0, `0x480`, `0x40`, and `0x480`. A nonzero byte is literal; `00 NN` means `1+NN` zero bytes. Outbound compression is used when it does not expand the body. Raw decoded length and reserved bytes are ignored on receive and emitted as zero. Frames are capped at 2048 bytes; malformed streams terminate until reset. Socket queues hold at most 16 frames per peer. The command queue permits 64 ordinary requests plus ten reserved, coalesced lifecycle entries, so teardown and the latest membership snapshot remain deliverable under saturation.

Protocol facts were pinned to:

- [spacemeowx2/ldn_mitm, 9a2639eff5b2e60065be3dfae69c46de6c3d7648](https://github.com/spacemeowx2/ldn_mitm/tree/9a2639eff5b2e60065be3dfae69c46de6c3d7648), especially `ldn_mitm/source/lan_protocol.hpp`, `lan_protocol.cpp`, and `ldn_types.hpp`. The project states GPLv2 with named special exceptions for yuzu/Ryujinx; those exceptions are not assumed to cover Eden.
- [Ryujinx contributors, c14ce4d2a5c9b373fb454906a6dc142c028d7be2](https://git.axenov.dev/Museum/ryujinx/commit/c14ce4d2a5c9b373fb454906a6dc142c028d7be2), especially `src/Ryujinx.HLE/HOS/Services/Ldn/UserServiceCreator/LdnMitm/LanProtocol.cs`. The reference's root `LICENSE.txt` is MIT.

The Eden codec is original C++ written from these wire facts, with original synthetic regression vectors. Reference implementation bodies were not copied, translated, or adapted. New Eden source uses GPL-3.0-or-later, consistent with this fork. Protocol authors and contributors are credited in source comments.

The standalone codec probes are `GoldenFrames`, `CompressionEdges`, `MalformedFrames`, and `TcpFragmentation` in `src/tests/core/ldn_mitm_codec.cpp`. `tools/ldn-mitm/codec_oracle.py` independently compares saved literal fixtures. CLI checks and opt-in real-frontend lifecycle/error probes are in `src/tests/control/test_ldn.py`; the CLI checks alone do not establish listener, persistence, guest LDN, GUI, or gameplay behavior.
