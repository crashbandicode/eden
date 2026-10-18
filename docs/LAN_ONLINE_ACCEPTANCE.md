# Automatic LAN Online validation — 2026-10-18

This report covers the Linux preview built from source snapshot3, corresponding to commit `c415e0e955c1f5acf4fe9b8f6d94d48d4f69d667`, with private companion runtime v6. The functional scope below is accepted for this preview; installation is tracked by separate host receipts. Later Windows source changes do not inherit these results.

## Identity

| Artifact | SHA256 |
|---|---|
| Frozen Eden source content | `6ebe4f66c17956eaa736b85fb6b4e03cec10e61f5cbd9d44ef570fd3f15098df` |
| Eden executable | `706f1ef9a21b9e171f980d3e4b87cbed46e4c56e33687e2b1c6d20ef58393e41` |
| Companion executable | `93bcfcb001c744eb3f21d27faba23c9641a2c4609129f4b67b9fc638ba48e2c2` |
| AppImage | `4f140652161a460d2e488cd0030a76b1d2019305adcc7f7893e4d06c0e641fab` |

The binary reports its original snapshot build identity, based on `41ebceb4`, because the feature was committed after that frozen build. Source bytes were verified before the commit. The private package contains support replies required by the game; these are not included in public source.

## Observed game behavior

Tests used Smash Ultimate 13.0.5, isolated writable profiles and private Linux rootful Xwayland displays. Profiles included the recorded baseline Deluxe/training mods; these results do not establish coverage of every mod release. No physical Switch, Nintendo account, or real NSO connection was used.

| Mode or transition | Observation |
|---|---|
| Automatic startup | Each Eden started its own companion and reached Online without a manual fixture or fixed server |
| Unmatched Quickplay | One instance remained waiting for 720 seconds, then matched a later peer; finite evidence, not proof of literal infinity |
| Same-PC singles | Two isolated instances completed results, accepted direct rematch and played a second complete match |
| Cross-PC singles | Two Linux machines discovered and matched, completed a round and accepted direct rematch into a second playable round |
| Background matchmaking | A lone background search paired with a normal Quickplay search; two rounds completed with direct rematch and the resume-search prompt |
| Co-op doubles | Two controllers per instance, four fighters; results, direct rematch, second playable match and return to character selection |
| Arenas | Linux keyboard naming, public search, join, ring readiness, two complete matches, matching results, return to arena, guest leave, host close, and no arena in a fresh search after closure |
| Cancellation and rejoin | Waiting search canceled to character selection; later requeue worked. A fresh peer also matched after coordinator-loss recovery |
| Coordinator loss | Survivor retained its instance identity and took over coordination; the game required explicit Online re-entry before a fresh peer could match |
| Cleanup | Final singles, doubles and arena runs exited normally before their bounds, with no OOM kills. Displays were stopped after game exit |

Same-PC guest Unix datagram descriptors and private runtime endpoints were observed during play. Per-packet counters were not measured in that game run; routing is additionally supported by source, socket tests and real-companion interoperability probes. Cross-PC plaintext and optional-TLS transport probes separately passed on two physical Linux hosts.

## Component checks and independent review

Configuration validation covered 33 cases, token behavior, 21 guest errno mappings and ordinary UDP while disabled. The socket probe passed 20 cases. Seven descriptor-lifetime probes passed normally and under AddressSanitizer, with deliberate mutations failing. Runtime v6 passed all eight module packages, including startup-budget and registration protections. Real C++/companion byte exchange and cleanup passed. These counts have different scopes and must not be summed into one test total. No Go race-detector run is claimed.

Independent Opus 5.5 reviews closed the recorded Linux blocking transport, integration and startup findings for this source/runtime pair. The final arena milestone review found no new blocking security issue. The reviewer recommended stronger evidence for a clean second cross-PC completion, live packet counts and Surface performance; those remain unaccepted rather than being claimed by this preview. The rematch gate establishes a second playable match, and the transport evidence distinguishes observation from inference. The Windows port has separate source-bound tests and an open security-review gate; it has not passed live Windows gameplay validation.

## Limits and retained failures

- Early drivers caused inactivity disconnections or excessive self-destructs. Those attempts are retained, not counted as passes. In the later cross-PC direct rematch, the second round reached sudden death, then the input driver expired; a roughly 79-second gap preceded a communication error. Yoga also reached its 8 GiB cap with 8,896 memory-pressure events. That second round's clean completion is not accepted; neither confound establishes or rules out a transport cause.
- Arena readiness must be confirmed on both peers within the game's countdown. A missed confirmation caused one earlier attempt to return to the waiting area. Short B taps manipulate tokens; hold B to reach the leave/close prompt, canceling token movement first if needed.
- Performance is host-dependent. Same-PC Yoga runs reached 60 FPS, while Surface cross-PC captures were often below full speed under host load. This is functional transition coverage, not a throughput or latency benchmark.
- Some runs reached the 8 GiB memory cap and incurred reclaim pressure without OOM. Launching two instances requires sufficient additional memory; a free slot alone is insufficient.
- Coordinator loss can require game reconnection. A virtual-address collision when previously separated groups reconnect can terminate a companion and require restarting Eden. Seamless partition recovery is outside this preview.
- The Linux native socket layer retains a documented Poll-versus-Close descriptor race: a concurrent poll can observe stale or reused readiness. The review classified it as nonblocking for this preview; it is not claimed fixed by the Windows lifetime repair.
- Background tests covered background Solo paired with normal Quickplay, not two simultaneous background searches or background Co-op. Arena teams were not tested. Public/domain-hosted lobbies and real account/subscription behavior are outside scope.

See [LAN_ONLINE.md](LAN_ONLINE.md) for operator and agent setup, optional TLS boundaries, and lifecycle instructions.
