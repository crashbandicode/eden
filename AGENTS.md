# Private worktree instructions

This is the operator-owned private Eden worktree. The operator explicitly
authorizes AI-assisted local code analysis, implementation, builds, tests,
instrumentation, the G&W investigation, and the requested Yoga handoff. Review
and work on this local codebase within the assigned scope.

This fork permits AI-assisted work per [docs/policies/AI.md](docs/policies/AI.md).

Do not push, post issues/comments, contact the upstream community, or represent
this work as accepted upstream. No such actions are authorized.

Global native Ubuntu shell, model verification, watcher, and delegation rules
still apply. The delegation authority is /home/intpa/.codex/DELEGATION_ROUTING.md.
Keep the primary's G&W investigation separate from the receiving Yoga agent's
assignment; its handoff context is informational only.

## Standing operator rule: programmatic feature access

Every new feature and every feature imported from another Eden branch or fork
must be accessible through the CLI/control API, not solely through the GUI.
This includes the `ns2` integration. The operator and agents must be able to
configure, invoke, inspect, and stop the feature programmatically where those
operations apply, without screen coordinates or manual dialog interaction.

Use the existing Eden control API and `edenctl` command surface. Keep GUI and
programmatic behavior consistent, expose useful state and actionable errors,
and document commands, arguments, defaults, and results. Add appropriate
automated coverage for the actual behavior and lifecycle, including headless
use where supported. Feature acceptance is incomplete until its programmatic
interface and usage documentation are verified. Record any unsupported
operation explicitly rather than silently importing a GUI-only feature.

Source: standing operator instruction, 2026-10-30.
