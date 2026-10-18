# Automatic LAN Online (Linux preview)

LAN Online is an Eden feature with a bundled companion runtime. It does not require a new NRO. It provides local test services for Smash Ultimate's Online interface; it is separate from local wireless/LDN and does not connect to Nintendo's online service.

The Linux preview has passed the live workflows recorded in [the acceptance report](LAN_ONLINE_ACCEPTANCE.md), with the limits stated there. Windows remains under validation. The older explicit fixture mode has separate instructions in [NSO_LAN_FIXTURE.md](NSO_LAN_FIXTURE.md). Passing configuration or socket tests alone does not establish playable matchmaking.

## Operators

The intended private Linux package contains Eden, `usr/bin/eden-lan-runtime`, and nine private game-support reply files under `usr/share/eden/lan-online/init`. Ordinary source builds do not contain those private inputs. Eden checks for the runtime and support files before loading a game with this mode enabled.

Enable **Emulation → Configure → Network → LAN Online** and leave Airplane Mode off. Restart Eden after changing this mode. Each Eden starts and owns its service automatically when it loads the supported game. Do not start a separate fixed-IP companion or set `EDEN_NSO_LAN_CONFIG` for automatic mode.

Use the same compatible Eden/runtime package and game version on each instance. Multiple instances on one PC require separate writable Eden profiles. They receive different identities and host ports automatically; identical guest game ports are allowed. Discovery and gameplay use Unix sockets between local instances. Cross-PC discovery and control use the network, and gameplay uses each instance's own UDP port. Same-PC Unix discovery is currently scoped to the same operating-system user.

For one PC, start two isolated profiles from the complete package; each owns its own runtime. For two PCs, start one instance on each PC on the same LAN. A dedicated server machine is unnecessary: the running instances select a coordinator. Do not configure router port forwarding for this LAN preview. If discovery fails, check the selected network interface and the local firewall; do not disable the firewall globally. A VPN can change the selected route. Keep **Pause emulation when in background** disabled for both instances; this Eden setting is separate from the game's Background Matchmaking mode.

Before loading the game, confirm that each profile resolves the base game and its actual installed update. Copying a profile between machines can leave its update-directory path pointing to the old machine. The title/version displayed by the running game is the check, not its filename. Connect one controller per instance for Solo, or two per instance for Co-op; each local player needs a separate controller assignment.

Enter the game's normal Online menus:

- **Quickplay:** choose Solo for singles or Co-op with two connected controllers for doubles. The required behavior is to remain waiting without an opponent and pair when a compatible player joins.
- **Background Matchmaking:** start the game's background search and continue through its normal prompts when paired.
- **Battle Arenas:** create an arena on one instance, then choose Public Arenas on another and join the named arena. Move both player tokens into the ring, start with Plus, then confirm A on both instances within the ready countdown. Names use the game's keyboard applet. Linux naming has passed on an isolated rootful Xwayland display without a window manager; this does not establish that every headless display setup works.
- **Results/rematch:** use the game's ordinary result screens and rematch choices. A playable second match is required evidence; a notification or successful server call alone is insufficient.

The acceptance report records the exact combinations tested for these workflows. Local-only voice and real account/subscription functionality are outside this preview.

Closing Eden stops its owned runtime. Another runtime can take over coordination when the coordinator disappears; existing game/NEX sessions may need to reconnect. Seamless recovery of an interrupted match is not promised. If separately operating LAN groups reconnect and two instances have the same virtual address, a companion can exit with a lease conflict; restart the affected Eden to obtain a fresh identity.

To leave an arena, hold B and confirm Yes. If a token is being picked up or moved, cancel that action and hold B again. The host receives a shutdown prompt; after confirming, a new public search should no longer find the arena. Short B taps can manipulate tokens without leaving.

### Windows (port under validation)

The Windows package places `eden-lan-runtime.exe` beside `eden.exe` and the nine private support replies under `lan-online\init`. Eden keeps each instance's runtime files below `%LOCALAPPDATA%\eden-lan`, a directory it creates with access for the current account and SYSTEM only. Same-PC instances still use Unix sockets: Windows AF_UNIX offers streams only, so each guest socket holds one AF_UNIX stream to its runtime carrying the same 24-byte framed datagrams, and the runtime authenticates every local connection by its peer process. Cross-PC discovery, lobby/control and UDP gameplay retain the Linux wire protocol. Windows/Linux gameplay interoperability remains an acceptance gate until live testing passes; Windows results are recorded separately.

## Agents and isolated validation

Do not reuse an active agent's profile, control socket, display or runtime directory. Check current processes, memory and slot ownership immediately before launch. Use one private display per game and the repository's bounded `tools/eden-run.py` launcher, with explicit memory, zero-swap and runtime limits. Record the binary hash, source snapshot, profile, display, service and control socket for each launch.

For an unpackaged development build only, `EDEN_LAN_RUNTIME` can name the runtime executable and `EDEN_LAN_ASSETS` can name the private support directory. These overrides are not required for a complete package. Do not print private reply contents, account tokens or keys into reports. Do not turn the explicit legacy fixture on at the same time.

Required Linux gates are same-PC Unix gameplay, cross-PC gameplay, late joining after sustained unmatched waiting, cancellation/rejoin, singles results/rematch, background matchmaking, Co-op doubles, arena create/browse/join/play/leave/close, coordinator loss, and normal disabled-mode regression. Every major milestone also requires an independent Opus 5.5 audit for obvious network-security issues. Retain raw evidence and mark untested paths as unverified. Windows implementation may proceed in parallel; each platform still needs its own acceptance before installation.

Keep both peers receiving varied valid gameplay input throughout live tests. Inactivity and excessive self-destructs cause the game to disconnect; retain the displayed reason before attributing a failure to networking. Result/rematch prompts are timed. Capture the actual prompt, confirm the intended answer within its countdown, then verify a second playable match on both peers. Controller RPC acceptance alone does not prove that the game consumed it. On cleanup, wait for each Eden process and its owned runtime to terminate before stopping that instance's private display.

## Optional lobby encryption

This preview targets a trusted local network. Unix endpoints are private to the current user and instance. The **Encrypt LAN lobby connections (TLS)** checkbox encrypts cross-machine lobby/control connections with TLS 1.3. It defaults to off. Use the same choice on every instance, including instances on the same PC, and restart Eden after changing it. Different choices form separate matchmaking groups and cannot find each other.

TLS protects against passive eavesdropping on the network. It does not verify who runs the lobby: the certificate fingerprint comes from unauthenticated LAN discovery. There is no silent fallback to plaintext. Same-host control and gameplay retain Unix sockets, and cross-machine gameplay retains UDP; this checkbox does not encrypt gameplay packets. The game's own NEX TLS handshake is separate. Public/domain-hosted lobbies remain future work and require hostname/certificate verification.

The TLS implementation has isolated test and independent review evidence. Real-LAN enabled/disabled transport probes have passed on Surface and Yoga. This is transport evidence; the game-mode observations and their limits are recorded separately in the acceptance report.
