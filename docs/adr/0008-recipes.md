# 0008 — Recipes for what apps' buttons do

- Status: Accepted (owner, 2026-10-04: "try it and then do the recipes; document them")
- Date: 2026-10-04

## Context

"Start the blockchain node" opened the app but could not start the node. The
start uses a configuration the app chose during setup.

Two ways to reuse the app's own logic were checked:
- **Reading its QML:** the official apps' buttons call a C++ backend whose
  logic is compiled.
- **Calling that backend by name:** tested in a scratch Basecamp v0.3.1. App
  backends are not published to other modules; the call failed with
  `object_unavailable` (docs/recipes.md).

## Decision

**Recipes:** per-app files compiled into the core (`core/recipes/*.inc`),
declaring actions as fixed steps.
- Required facts are read from the app's own settings under `~/.config/Logos`.
- Steps can skip early ("already running") and pass results between calls.
- Per-call timeouts.

The model chooses an action with the new `recipe {app, action}` tool. The core
opens the app if needed and runs the steps; the user confirms the exact calls
first.

First recipes:
- **Blockchain:** start and stop, using the config the app saved.
- **Storage:** start (load, init and start, in the order that keeps the user's
  saved settings) and stop.

## Rejected

- **Letting the model chain calls itself.** It cannot know the saved paths, and
  passing one call's result to the next is fragile in a small model.
- **UI automation.** Not available in v0.3.1, and brittle.

## Consequences

- **Recipes are knowledge we maintain.** They need checking when an app
  changes; each records its source and version.
- **The right long-term mechanism is app intents** (`provides` such as
  `blockchain.node.start`). To propose upstream.
