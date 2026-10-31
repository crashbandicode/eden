# Agent instructions

This fork permits AI-assisted work per [docs/policies/AI.md](docs/policies/AI.md).

Pushes, repository visibility changes and external communications require the
operator's authorization. Do not represent this fork's changes as accepted
upstream. Follow applicable local operator instructions in addition to this file.

Keep private handoff/progress notes, credentials, game files and captured service
replies outside Git. Source publication does not establish runtime acceptance;
record the actual platform, revision and scope of tests and any remaining gaps.

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
