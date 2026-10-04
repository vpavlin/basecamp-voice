# 0009 — Asking apps through Basecamp's intents

- Status: Proposed (owner asked, 2026-10-04: "analyze scala and create intents for that")
- Date: 2026-10-04

## Context

ADR 0008 and docs/recipes.md said the proper fix for "do what the app's button
does" was app intents, and that it needed proposing upstream. That was wrong.
**Basecamp 0.3.1 already ships app-to-app intents** (logos-basecamp
`docs/app-to-app-intents.md`; we use one of the shell's, `basecamp.apps.launch`):

- **Providing:** a `ui_qml` app lists what it can do under `provides` in its
  `metadata.json`, with typed `params` (`string`, `number`, `bool`, `object`,
  `array`). Its view handles `onIntentRequested(requestId, intent, params,
  requester)` and answers once with `logos.respond(requestId, ok, data, error)`.
- **Asking:** another app calls `logos.request(intent, params, cb)`. It must
  list the intent under `uses` first.
- **Basecamp in between:**
  - asks the user to confirm, every time;
  - brings the provider forward;
  - checks params against the provider's declaration;
  - returns `{ok, data, error}`. `error` is one of six codes and never the
    provider's text.

The first app to provide intents is Scala, the calendar (vpavlin/scala, its
ADR 0022): list calendars, list/search events, add an event, create, join or
share a calendar, show a date.

## Decision

A new tool, `intent {intent, params}`.

- **What the model sees.** Intents come from installed apps'
  `metadata.json` (found through `installDir` from
  `package_manager.getInstalledPackages`). Only intents our own view lists in
  `uses` count; Basecamp refuses the rest anyway. The context lists each with
  its description and params (`?` = optional), plus the current local time,
  so "on Tuesday at 3" can be turned into `2026-10-06T15:00`.
- **What the model can write.** The plan schema narrows the tool to exactly
  those intents, each with its own params and types.
- **Checking a step.**
  - Required params must be there, and each value must have its declared type.
  - Empty optional values are dropped, and so are params the app does not
    describe.
  - The plan shows "Ask scala_ui: Add an event to a calendar (title: Dentist,
    start: 2026-10-06T15:00)".
- **Our own confirmation.**
  - Intents the app marks `"readOnly": true` (our documentation key; Basecamp
    ignores it) run without our **Do it**. Their answer feeds a follow-up round,
    like a listing.
  - Everything else waits for **Do it**.
  - Basecamp's own dialog appears either way: that consent is the shell's, and
    a requester cannot skip it.
- **Raising it.**
  - The core hands the intent to the view (the existing view action that opens
    apps), and the view calls `logos.request`.
  - The wait is up to 11 minutes once the view has taken it: the user answers
    the dialog, the app may load, and Basecamp's own backstop is 10 minutes.
  - If the window is closed, the step fails after 20 s, as opening an app does.
- **Showing the answer.**
  - An array in `data` (events, calendars) becomes the numbered list, the same
    for the user and the model: "1. Dentist (2026-10-06 15:00–16:00, Team)".
  - An object becomes the summary.
  - A refusal code becomes a sentence. For example, `bad_request` becomes
    "scala_ui refused …; its window says why", because Scala explains in a toast.
- **`uses` lists Scala's eight intents by name.** Basecamp requires the
  names, so each new app's intents need a line in `view/metadata.json` and a
  new release of the view.

## Rejected

- **Calling `scala` core methods (`call`)** instead:
  - the model would need ids, epoch-millisecond strings and double-encoded JSON;
  - `listCalendars` hands out each calendar's encryption key;
  - the view's choice of signing identity would be bypassed;
  - the user would not see Scala do it.
- **A recipe for Scala:** recipes are for apps that cannot answer for
  themselves. Scala can.

## Consequences

- Any app that adopts `provides` becomes speakable, once its intents are in our
  `uses`. Scala's ADR 0022 is the worked example.
- Two confirmations for writes: our **Do it**, then Basecamp's dialog. A read
  shows only Basecamp's, and the provider comes forward briefly.
- **Upstream ask, replacing the old one.** An assistant cannot know every
  intent at build time. Basecamp would need either a way to declare "may
  request any intent the user confirms" (for example `uses: [{"intent": "*"}]`,
  allowed only with the confirmation always shown), or `uses` read at request
  time. Not filed; needs the owner's go-ahead.
- **App authors:** each `provides` entry should carry a `description` (on the
  entry and on each param), and `readOnly: true` where it changes nothing. These
  documentation keys pass through Basecamp untouched.
- **Tested:**
  - unit tests (`core/tests/test_intents.cpp`, mutation-checked);
  - Scala's offscreen harness (`scala-ui/qml-harness/intents.sh`);
  - the model on seven calendar cases in `core/eval/cases.json`: 6 of 7
    before a prompt rule for "when is …". It has not been re-run since,
    nor have the existing 43 cases.

  **Not yet run in a real Basecamp.**
