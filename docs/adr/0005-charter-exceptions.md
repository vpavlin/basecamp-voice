# 0005 — Which logos-app-charter rules do not apply, and why

- Status: Accepted
- Date: 2026-10-03

## Context

logos-app-charter's rules are written for household multi-writer, offline-first
apps. Its "when to bend a rule" section asks for an ADR whenever an app
genuinely isn't that. basecamp-voice is a **single-device tool**: it drives the
Basecamp it runs in. It shares no state with other people or devices, and has
no wire contract.

## Decision

These rules do **not** apply, because there is nothing to sync:
- 1, Android parity
- 2, Loam
- 4, converge later
- 5, event-log CRDT
- 6, household sealing
- 11, same versions everywhere

Local-first (rule 3) applies in its single-device form:
- **The model is local by default.** Speech and the model run on this machine
  unless the user points the endpoint elsewhere.
- **No network except on demand.** The only network traffic is package
  downloads and the one-time runtime/model fetch the user confirms.

These rules **do** apply:
- 7: seams for the speech-to-text engine and the model endpoint.
- 9: publishing discipline.
- 10: never block; plain text; every outcome visible.
- 13: ADRs.
- 14: verify on the real path.

Rule 12 (every platform Basecamp ships) applies **later**. The fetched
runtimes (`llama-server`, `parakeet-cli`) are per-platform binaries. Ship Linux
x86_64 first, say so, and add platforms through `logos-multiplatform-modules`.

## Consequences

- **Revisit if the app starts sharing anything** (for example, sharing
  "recipes" of spoken commands).
