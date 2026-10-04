# 0004 — The model's tool set, and how the core checks it

- Status: Accepted
- Date: 2026-10-03

## Context

A small local model is reliable with a short, well-described tool set, and the
core must stop it from doing anything outside that set. The spike found:
- a misspelled method returns `ok` with a `null` result;
- calling a module that isn't loaded blocks for the whole call timeout;
- `getPluginMethods` / `getPluginEvents` are the introspection that works
  (`getMethods` returns nothing);
- many modules return `{"ok":false,"error":…}` *inside* a successful result.

## Decision

The tools:

| Tool | Backed by |
|---|---|
| `list_installed()` | `package_manager.getInstalledPackages` |
| `list_available(query?)` | `package_downloader.getCatalog`, filtered and trimmed in the core |
| `install(name)` | `package_downloader.downloadResolvedDependencies` → `package_manager.installPlugin` per row, in order |
| `open_app(app)` | install if needed → view raises `basecamp.apps.launch` → wait for `modules_state.is_ready` on its cores |
| `module_methods(module)` | `getPluginMethods` (+ `getPluginEvents`) |
| `call(module, method, args[])` | `dynamic(module).invoke(...)` |
| `status()` | `modules_state.list_modules` |

How the core checks a `call`:
1. **Refused unless the module is ready.** The core checks
   `modules_state.is_ready` (about 1 ms) first, so it never waits out a timeout.
2. **Refused unless the method exists with that arity,** checked against
   cached `getPluginMethods`.
3. **Result reported as the model sees it.** The result is unwrapped (up to
   two JSON layers), and an inner `{ok:false,error}` is reported to the model
   as a failure.

> **Amended 2026-10-03 after the code review:**
> - **Every `call` needs confirmation,** whatever the method is called. The
>   name heuristic below was injectable: plans can come from text strangers
>   wrote, such as package descriptions in the model's context, and names like
>   `get_or_create_…` pass it.
> - **Calls to Basecamp's own modules are refused:** capability_module,
>   package_manager, package_downloader, modules_state and basecamp_voice_core.
>   Those are reached only through the tools.

> **Added 2026-10-04:**
> - **`recipe {app, action}`** (ADR 0008).
> - **`intent {intent, params}`** (ADR 0009): ask an installed app through
>   Basecamp's own intents; Basecamp confirms every one with the user.
> - **`add_repository {url}`:** https only, always confirmed, and the plan warns
>   that its packages may be unsigned.
> - **`list_available` takes an optional `repository`,** using
>   `package_downloader.getCatalogForRepo`.
>
> The raw `call` into package_downloader stays refused; these tools are the
> only way in.

Consent: **plan first, then act.** Basecamp does not prompt when a module
installs packages, so this is the only safeguard.
- `install`, `open_app` and any `call` not marked read-only wait for the user's
  confirmation of the plan.
- Read-only means `list_*`, `module_methods`, `status`, and calls to a method
  whose name starts with `get`, `list` or `is`. That list is to be refined
  per module.

## Rejected

- **Free-form code or arbitrary IPC from the model.** Nothing it produces runs
  except through these tools.

## Consequences

- **Large installs report progress.** package_downloader emits
  `downloadProgress`, and calls use long timeouts (Basecamp's own UI uses
  5 min). `blockchain_module` is 148 MB.
- **The read-only heuristic is a guess.** It is the first thing to tighten
  after real use.
