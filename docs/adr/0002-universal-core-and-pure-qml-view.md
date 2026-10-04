# 0002 — A universal core module and a pure-QML view

- Status: Accepted
- Date: 2026-10-03

## Context

The brief proposed a legacy Qt core module, because only those could call
modules unknown at build time. At v0.3.1's cpp-sdk, universal (Qt-free) cores
get `modules().dynamic(name)`, an `LpClient` that can call any module by name.
The spike used it to call package_manager, package_downloader, modules_state
and a freshly installed module, none of them declared as dependencies.

`vpavlin/logos-skills` (logos-basecamp-module, logos-app-charter) prescribes
the house shape: a universal core that owns all logic, and a thin pure-QML
view with no C++ backend.

## Decision

- **`basecamp_voice_core`: universal core module, ASCII-only metadata.**
  - Owns everything: recording, speech-to-text, the model client, the tool
    executor, the job/event log.
  - Calls Basecamp modules through `modules().dynamic(name)`.
  - Long work runs on background threads.
  - Public methods return `{ok, error, …}` JSON strings, take at most 4
    arguments, and never throw across IPC.
- **`basecamp_voice`: pure-QML `ui_qml` view.**
  - Built with `logos.callModuleAsync` only, through one helper. Present in
    v0.3.1's bridge as `callModuleAsync(module, method, args, cb, timeoutMs)`.
  - Polls are single-flight: one request in flight at a time.
  - `Text.PlainText` on every text item. Transcripts and module output are
    untrusted text.
  - Uses the `Logos.Controls` / `Logos.Theme` design system. It is linked into
    v0.3.1, and the catalog's `eth_rpc_ui` uses it and loads. Use components
    present at design-system rev `88330d0`.
- **What the view adds beyond rendering: opening apps.** It performs the shell
  intents the core asks for (ADR 0003), because intents are a QML-bridge API.

## Rejected

- **A legacy Qt core.** It's no longer needed for by-name calls, and it's off
  the house path.
- **A ui_qml view with a C++ backend.** That is the combination the skills
  record as failing to open on some Basecamp builds, and it adds nothing here.

## Consequences

- **The core is reachable only through by-name calls.** Under
  `--access-policy enforce`, by-name calls to undeclared modules would be
  refused. Not tested; enforcement is off by default in v0.3.1.
- **Intents live only in the view.** If no view is open, nothing can open an
  app. The view is the product, so that's acceptable.
