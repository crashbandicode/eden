# Automatic LAN Online (Linux preview, under validation)

LAN Online is an Eden feature with a bundled companion runtime. It does not require a new NRO. It provides local test services for Smash Ultimate's Online interface; it is separate from local wireless/LDN and does not connect to Nintendo's online service.

This implementation is under validation. The older explicit fixture mode has separate instructions in [NSO_LAN_FIXTURE.md](NSO_LAN_FIXTURE.md). Passing configuration or socket tests does not establish playable matchmaking. Consult the release's acceptance report before relying on a particular game mode.

## Operators

The intended private Linux package contains Eden, `usr/bin/eden-lan-runtime`, and nine private game-support reply files under `usr/share/eden/lan-online/init`. Ordinary source builds do not contain those private inputs. Eden checks for the runtime and support files before loading a game with this mode enabled.

Enable **Emulation → Configure → Network → LAN Online** and leave Airplane Mode off. Restart Eden after changing this mode. Each Eden starts and owns its service automatically when it loads the supported game. Do not start a separate fixed-IP companion or set `EDEN_NSO_LAN_CONFIG` for automatic mode.

Use the same compatible Eden/runtime package and game version on each instance. Multiple instances on one PC require separate writable Eden profiles. They receive different identities and host ports automatically; identical guest game ports are allowed. Discovery and gameplay use Unix sockets between local instances. Cross-PC discovery and control use the network, and gameplay uses each instance's own UDP port.

Enter the game's normal Online menus:

- **Quickplay:** choose Solo for singles or Co-op with two connected controllers for doubles. The required behavior is to remain waiting without an opponent and pair when a compatible player joins.
- **Background Matchmaking:** start the game's background search and continue through its normal prompts when paired.
- **Battle Arenas:** create an arena on one instance, then browse and join it from another. Names use the game's keyboard applet. Linux headless arena creation is an explicit validation gate because an older display setup crashed there.
- **Results/rematch:** use the game's ordinary result screens and rematch choices. A playable second match is required evidence; a notification or successful server call alone is insufficient.

These workflows describe the required behavior. Their acceptance status must be recorded separately for the automatic implementation. Local-only voice and real account/subscription functionality are outside this preview.

Closing Eden stops its owned runtime. Another runtime can take over coordination when the coordinator disappears; existing game/NEX sessions may need to reconnect. Seamless recovery of an interrupted match is not promised.

## Agents and isolated validation

Do not reuse an active agent's profile, control socket, display or runtime directory. Check current processes, memory and slot ownership immediately before launch. Use one private display per game and the repository's bounded `tools/eden-run.py` launcher, with explicit memory, zero-swap and runtime limits. Record the binary hash, source snapshot, profile, display, service and control socket for each launch.

For an unpackaged development build only, `EDEN_LAN_RUNTIME` can name the runtime executable and `EDEN_LAN_ASSETS` can name the private support directory. These overrides are not required for a complete package. Do not print private reply contents, account tokens or keys into reports. Do not turn the explicit legacy fixture on at the same time.

Required Linux gates are same-PC Unix gameplay, cross-PC gameplay, late joining after sustained unmatched waiting, cancellation/rejoin, singles results/rematch, background matchmaking, Co-op doubles, arena create/browse/join/play/leave/close, coordinator loss, and normal disabled-mode regression. Every major milestone also requires an independent Opus 5.5 audit for obvious network-security issues. Retain raw evidence and mark untested paths as unverified. Windows implementation follows Linux acceptance.

## Optional lobby encryption

This preview targets a trusted local network. Unix endpoints are private to the current user and instance. The **Encrypt LAN lobby connections (TLS)** checkbox encrypts cross-machine lobby/control connections with TLS 1.3. It defaults to off. Use the same choice on every instance, including instances on the same PC, and restart Eden after changing it. Different choices form separate matchmaking groups and cannot find each other.

TLS protects against passive eavesdropping on the network. It does not verify who runs the lobby: the certificate fingerprint comes from unauthenticated LAN discovery. There is no silent fallback to plaintext. Same-host control and gameplay retain Unix sockets, and cross-machine gameplay retains UDP; this checkbox does not encrypt gameplay packets. The game's own NEX TLS handshake is separate. Public/domain-hosted lobbies remain future work and require hostname/certificate verification.

The TLS implementation has isolated test and independent review evidence. Real-LAN enabled/disabled probes and game acceptance remain required before this preview can be released.
