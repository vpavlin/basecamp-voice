# 0001 — Target Basecamp v0.3.1

- Status: Accepted
- Date: 2026-10-03

## Context

The Basecamp APIs this app drives — the package catalog, install, module state,
the shell's intents — changed substantially between the March workspace
checkout and v0.3.1 (FINDINGS.md, Q4). Upstream main was already 20 commits
past v0.3.1 on the day of the spike, and changes package_downloader's lifecycle
(`5fc0720`, lazy start with a new `start()`/`getState()`).

## Decision

Build and test against **Basecamp v0.3.1** (`aeb8192`), which the owner expects
to remain the stable release for a while.

- Read APIs from the sources pinned by v0.3.1's `flake.lock`, not the
  workspace. The revs are listed in FINDINGS.md.
- Build modules with logos-module-builder `4b79982`, the rev v0.3.1's own
  package_downloader was built with. It uses the same cpp-sdk and
  logos-protocol as the host.
- Test in the real v0.3.1 AppImage with `--user-dir <scratch>`, offscreen,
  never against the owner's data dirs.

## Rejected

- **Tracking upstream main.** Its package APIs are still moving, and it isn't
  what users run.

## Consequences

- When a newer Basecamp becomes the stable release, the package calls
  (`package_downloader`, `package_manager`, `modules_state`) are the first
  thing to re-verify. Keep them behind one adapter in the core so the change
  stays in one place.
