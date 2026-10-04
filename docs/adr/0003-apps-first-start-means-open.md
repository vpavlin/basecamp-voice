# 0003 — Apps first: "start X" means open X's app

- Status: Accepted
- Date: 2026-10-03

## Context

In v0.3.1, installing a core module does not load it. Nothing loads it on
demand, and no IPC call lets a module load another (FINDINGS.md, Q1). What
does load a core: opening a view that depends on it. The spike raised
`basecamp.apps.launch {app}` from the view; the shell replied `ok` with no
prompt, opened the app, and its core was `ready` about 2 s later.

## Decision

The app works with **UI modules (apps)**. Users ask for apps, and "start X"
means "open X's app". The core resolves a spoken name to an app (`*_ui`, or
any `ui_qml` package) and:

1. installs it if missing; its core dependencies come with it;
2. asks the view to raise `basecamp.apps.launch {app}`. The view declares it in
   `metadata.json`: `"uses": [{"intent": "basecamp.apps.launch"}]`;
3. waits on `modules_state.is_ready` for each of the app's core dependencies
   before calling any of them.

A core module with no app is out of scope. If asked for one, the assistant says
plainly that it has no app, and that the user can load it from Basecamp's
Modules view.

## Rejected

- **Asking upstream for a module-load API or intent.** Not needed for an
  apps-first product.
- **Requiring a Basecamp restart.** It doesn't load installed cores anyway.

## Consequences

- **The user watches each app open as it happens,** which is the product's
  intent.
- **Opening an app takes focus away from the voice view.** The view's progress
  must survive being in the background, and the summary must be there when the
  user comes back. Revisit once seen in the real GUI.
