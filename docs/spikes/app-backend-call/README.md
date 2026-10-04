# Can a module call an app's backend? (2026-10-04)

**No, not in Basecamp v0.3.1.**

Setup: a scratch Basecamp (`--user-dir`), with blockchain_ui and
blockchain_module copied in. The spike driver (`../install-and-call/voice_spike`)
opened blockchain_ui through `basecamp.apps.launch`, then called it by name.

| Step | Result |
|---|---|
| open blockchain_ui | `ok`; blockchain_module `ready` 1 s later |
| `modules_state.list_modules` | blockchain_ui is **not** listed |
| `blockchain_ui.getPluginMethods()` | `object_unavailable` after 120 s |
| `blockchain_ui.getTimeInfo()` (a read-only backend slot) | `object_unavailable` after 120 s |

Basecamp's log shows the backend being handed a token (`Informing module token
for module: "blockchain_ui"`), so it can call modules. It is not published as
something modules can call.

Consequence: recipes (`docs/recipes.md`, ADR 0008).
