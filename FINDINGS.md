# basecamp-voice — spike findings

2026-10-03. Answers the four open questions in BRIEF.md. The answers change
parts of the proposed design, so the decisions to take are listed at the end.
Nothing beyond the spike has been built.

## How this was established

- **Target: the installed AppImage, not the workspace.** Basecamp
  `LogosBasecamp-Desktop-v0.3.1-aeb819` was extracted to `/tmp/bcv/squashfs-root`.
  Its source is logos-basecamp `aeb8192` (2026-10-01). Every dependency was
  cloned at the rev in *that* commit's `flake.lock`:
  - package-manager-module `41df424`
  - package-downloader-module `d40d4ab`
  - liblogos `db45024`
  - cpp-sdk `c24c4ab`
  - capability-module `1a1b8b5`
  - modules-state-module `ed0f4ba`
  - package-manager-ui `3174edd`

  The workspace checkout (`~/devel/github.com/logos-co/logos-workspace/repos/`)
  is from **March 2026** and is obsolete for this purpose (see Q4).
- **The spike** is two packages in `docs/spikes/install-and-call/`:
  - `voice_spike` is a universal core module. It can call any module by name
    (`call(module, method, argsJson)`), install a package with its
    dependencies (`installByName`), and run a JSON script of steps on a
    background thread. Each step is logged to `/tmp/voice_spike/log.jsonl`.
  - `voice_spike_ui` is a pure-QML view. Opening it loads the core, and it
    raises the shell intents the core asks for.
  - Both are built with logos-module-builder `4b79982`, the rev Basecamp's own
    bundled package_downloader was built with (same cpp-sdk / logos-protocol as
    the host).
- **How it was run:** the real v0.3.1 binary, isolated and offscreen:
  ```
  QT_QPA_PLATFORM=offscreen ./squashfs-root/AppRun --user-dir /tmp/bcv/ud \
      --uri basecamp://app/voice_spike_ui
  ```
  - `--user-dir` gives the instance its own modules/plugins/module_data. The
    `--uri` opens the spike view, which loads the spike core, which runs
    `/tmp/voice_spike/autorun.json`.
  - I used the GUI rather than logoscore because the questions are about
    Basecamp's behaviour (its install listener, its app launcher), which
    logoscore does not have.
  - Your running instances were not touched.
- **Evidence:** scripts, the run-2 step log and Basecamp log excerpts are in
  `docs/spikes/install-and-call/evidence/`. Run 1's step log was overwritten by run 2; its key lines are
  quoted below from the session.
- **Packages used:**
  - `token_list_module`: core, no dependencies, 29 MB.
  - `eth_rpc_ui` + `eth_rpc_module`: view + core, 14 + 29 MB.
  - All three from the official catalog.

## Answers

### Q4 — Does v0.3.1 match what the brief assumed? **No. The package API was split and replaced.**

| Brief assumed | v0.3.1 has |
|---|---|
| `package_manager.getPackages(category)`, `getCategories()` | Catalog moved to a separate bundled module, **`package_downloader`**: `getCatalog()`, `getCatalogForRepo()`, `listRepositories()`, `refreshCatalog()`, `addRepository(url)` |
| `installPackage(name, dir)`, `installPackage(s)Async` | Two steps: `package_downloader.downloadResolvedDependencies(depsJson, installedJson)` → local `.lgx` paths in install order, then `package_manager.installPlugin(lgxPath, false, source)` for each |
| `resolveDependencies(names)` | `package_downloader.resolveDependencies(depsJson, installedJson)` (preview, no download) |
| — | `package_manager.getInstalledPackages()`, `uninstallPackage(name)` |
| — | New bundled **`modules_state`**: `list_modules()`, `module_record(m)`, `is_ready(m)`, event `module_state_changed` |

- **The old methods are gone from the binary,** not just from the source.
  - API contracts: `squashfs-root/usr/plugins/package_manager_ui/assets/lidl/{package_manager,package_downloader}.lidl`
    and `…/modules/package_downloader/assets/lidl/modules_state.lidl`.
  - `package_downloader_impl.h:14-25` says the legacy single-repo calls were
    removed.
- **Also changed: core modules are universal (Qt-free), with a by-name escape
  hatch.**
  - At this cpp-sdk, the generated `LogosModules` has
    `logos::LpClient& dynamic(const std::string& target)`
    (`cpp-sdk/cpp-generator/generator_lib.cpp:1952`).
  - That client calls *any* module by name: `invoke(method, jsonArgs, &err, timeoutMs)`
    (`cpp-sdk/cpp/logos_lp_client.h`).
  - So the brief's premise that the core must be a legacy Qt module to call
    modules unknown at build time no longer holds. The spike core is a normal
    universal module and calls everything through `dynamic()`.

### Q2 — Can a module call package_manager / package_downloader? **Yes, unrestricted by default.**

**Observed (run 1).** From `voice_spike`, with no `dependencies` declared:
- `package_manager.getInstalledPackages` ok, 6 ms.
- `package_downloader.listRepositories` ok, 9 ms.
- `package_downloader.getCatalog` ok, 163 ms, 243 KB of JSON.
- `modules_state.list_modules` ok.
- The full install (below) went through without any prompt.

**Why (source):**
- `logos-basecamp/app/main.cpp:363-383`: the inter-module access policy defaults
  to *none*, meaning "any loaded module may call any other".
- `capability_module_impl.cpp:119-129` is fail-open for any target without a
  registered restriction.
- Neither module checks caller identity. The manifests' `"capabilities"` field
  is not enforced anywhere.

**Caveat — enforcement can be switched on.**
- `--access-policy enforce` / `LOGOS_ACCESS_POLICY` switches to deny-by-default:
  a module may call only the modules it declares as dependencies
  (`liblogos/src/logos_core/module_manager.cpp:441-474, 1136-1166`).
- Under it, the voice core could reach package_manager / package_downloader /
  modules_state if it lists them in `dependencies`. It could **not** reach
  arbitrary modules through `dynamic()`.
- Not tested — it's off in a stock install.

**Not used: `requestInstall` / `confirmInstall` on package_manager.**
- That pair does not install anything.
- Basecamp does not listen for its `beforeInstall` event. Without an
  `ackPendingAction` it cancels itself after 3 s
  (`package_manager_impl.cpp:894-934, 1148-1182`).
- Only the logoscore CLI daemon uses it.
- Basecamp's own UI calls `installPlugin` directly
  (`PackageManagerBackend.cpp:770`).

### Q3 — Which install directory? **None — the module doesn't pass one.**

- **package_manager already knows the directories.**
  - `installPlugin` takes only the `.lgx` path.
  - Basecamp configures package_manager's directories at startup
    (`app/PackageCoordinator.cpp:121-124`, `setUserModulesDirectory(...)` etc.).
  - Cores go to `<user-dir>/modules/<name>`, views to `<user-dir>/plugins/<name>`.
- **Observed (run 2):**
  ```
  downloadResolvedDependencies('[{"name":"eth_rpc_ui"}]', <installed>)
    -> [{name: eth_rpc_module, path: /tmp/lgpd-1000/eth_rpc_module-…lgx, …},
        {name: eth_rpc_ui,     path: /tmp/lgpd-1000/eth_rpc_ui-…lgx, …}]      (dependency first)
  installPlugin(...) -> {isCoreModule:true,  path:/tmp/bcv/ud/modules/eth_rpc_module/eth_rpc_module_plugin.so, signatureStatus:"unsigned"}
  installPlugin(...) -> {isCoreModule:false, path:/tmp/bcv/ud/plugins/eth_rpc_ui/eth_rpc_ui_plugin.so}
  ```
- **Timing:** 6.2 s download + 2.6 s install for 29 MB in run 1. Run 2's
  dependency pair took about 80 s for 43 MB. Large packages
  (`blockchain_module` is 148 MB) need progress reporting and long timeouts;
  Basecamp's UI uses 5 min.
- **Basecamp notices immediately.** It logs `Core module file installed: …` and
  `UI plugin file installed: …`, refreshes its module list, and the new view
  appears as an app.

### Q1 — Can a module installed while Basecamp runs be called straight away? **No. Installing does not load it, and nothing loads it on demand. But "install and run" works in one go by launching the app.**

**Observed (run 1): `token_list_module`, a core with no view.**

| t | step | result |
|---|---|---|
| before install | `token_list_module.getMethods` | **blocked 120 442 ms**, then `object_unavailable: failed to acquire remote object 'token_list_module' (module not loaded, not published, or transport failure)` |
| after install | `modules_state.module_record` | `{"state":"unloaded", …}` |
| +2 s | call again | blocked 120 176 ms, `object_unavailable` |
| +2 … +32 s | `modules_state.is_ready` ×11 | `false` every time |
| after a Basecamp **restart** (run 2) | `module_record` | still `"state":"unloaded"`: nothing loads installed cores at startup either |

- Basecamp's log during the failed call:
  `no listener at "local:logos_token_list_module_…" -- request for "token_list_module" will block up to 120000 ms and then fail. Is the module loaded?`
- **The 120 s is the timeout the spike passed.** The call blocks for whatever
  timeout it is given. The protocol default is unknown — I didn't measure it.
- **Why (source):**
  - On `corePluginFileInstalled`, Basecamp only *rescans*
    (`PackageCoordinator.cpp:126-136` → `CoreModuleManager::refresh()` →
    liblogos `discoverInstalledModules()`). The module goes from absent to
    `unloaded`.
  - capability_module will not mint a token for a target that isn't loaded
    (`capability_module_impl.cpp:108-115`).
  - Nothing exposed over IPC can load a module:
    - capability_module only mints tokens.
    - modules_state is read-only.
    - logoscore's `core_service.loadModule` does not exist inside Basecamp.
  - Basecamp loads a core in three cases: at startup for its own two package
    modules; when the user clicks Load in the Modules view; and **when a view
    that depends on it is opened** (`PluginLoader.cpp:147-215`).

**Observed (run 2): install `eth_rpc_ui`, then launch it from the voice view.**

| t (from launch) | step | result |
|---|---|---|
| −2 s | `module_record eth_rpc_module` | `"state":"unloaded"` |
| 0 | view: `logos.request("basecamp.apps.launch", {app:"eth_rpc_ui"}, cb)` | `{"ok":true,"error":""}` in 470 ms. No chooser, no prompt. |
| | Basecamp log | `App launcher clicked: "eth_rpc_ui"` → `Loading core dependencies for "eth_rpc_ui" : QList("eth_rpc_module")` |
| +2 s | `modules_state.is_ready eth_rpc_module` | **`true`** (`state:"ready"`, pid assigned) |
| +2 s | `eth_rpc_module.getPluginMethods` | full method list with names, parameter types, return types |
| | `eth_rpc_module.getPluginEvents` | event list |
| | `eth_rpc_module.list_chains()` | `"{\"chains\":[],\"ok\":true}"` (1 ms) |
| | `eth_rpc_module.get_chain_config(1)` | `"{\"error\":\"no config for chain 1\",\"ok\":false}"`. An int argument passed as a JSON number works. |

**So "install X and start it" works without a restart, when X has a view.**
1. The core installs X's view; its dependencies come with it.
2. The voice **view** raises `basecamp.apps.launch`. The view has to declare
   the intent in its `metadata.json`:
   `"uses": [{"intent": "basecamp.apps.launch"}]`. Without it the reply is
   `not_declared` (observed in run 1). A bare string array is silently ignored
   (`docs/app-to-app-intents.md:166`).
3. Basecamp opens the app, which loads X's core.
4. The core waits on `modules_state.is_ready` (about 1 ms per call), then calls.

The user sees the app open, which fits "you watch it happen live".

**What it doesn't cover: a core module with no view.** Examples:
`token_list_module` installed alone, `delivery_module`, `storage_module`, the
many `*_module` packages whose only view is a separate package. I found no
programmatic way to load these in v0.3.1. Most official cores do have a
`*_ui` package that depends on them. `blockchain_ui` → `blockchain_module` is
the brief's example, so "install the blockchain module and start it" maps to
installing and launching `blockchain_ui`.

## Other things the spike surfaced (they affect the design)

1. **Never call a module that isn't ready.** An unloaded target doesn't fail
   fast; it blocks for the whole call timeout. Check `modules_state.is_ready(m)`
   first; it answers in about 1 ms and is safe to call often.
2. **Introspection is `getPluginMethods` / `getPluginEvents`, called as
   ordinary methods.** `LpClient::getMethods()` returned `[]` and
   `invoke("getMethods")` returned `null` against package_manager,
   capability_module and eth_rpc_module. `getPluginMethods` returns
   `[{name, parameters:[{name,type}], returnType, signature, isInvokable}]`,
   which is enough to build a `call` tool's schema for the model at runtime.
3. **A misspelled method fails silently.**
   `eth_rpc_module.no_such_method()` returned `ok` with a `null` result. The core
   has to check the method name and arity against `getPluginMethods` before
   calling, or a model's typo looks like success.
4. **Many modules report errors inside their result.**
   `{"ok":false,"error":…}` comes back as a *successful* call whose result is a
   JSON string, sometimes double-encoded. The tool layer should unwrap it and
   surface that inner `ok`/`error` to the model.
5. **The view is the only place that can open apps.** `logos.request` (intents)
   is a QML bridge API; the intent docs say providers are ui_qml apps and core
   modules call each other directly. So the split is: the core plans, installs
   and calls; the view opens apps when the core asks. The spike does this with
   `nextAction()` polling on a Timer, and it worked.
6. **Downloads land in `/tmp/lgpd-<uid>/`.** Packages are unsigned
   (`signatureStatus:"unsigned"`, logged as a warning). Basecamp's own UI
   installs them the same way.
7. **Builder quirks hit on the way.** A ui_qml must ship a 256×256 `icon`. A
   view that depends on a core needs that core as a flake input (for its LIDL);
   `path:../voice_spike` works. The builder does **not** copy `uses` into
   `manifest.json`, but Basecamp reads `uses` from the installed `metadata.json`,
   so it still works.
8. **The workspace is obsolete as a reference.** Its repos are from March. Work
   against the sources pinned by the target Basecamp's `flake.lock`; the clones
   used here are in `/tmp/bcv/src-*`.

## Decisions for the owner

> **Decided 2026-10-03.** The owner's answers are recorded in `docs/adr/`:
> - 0001: v0.3.1 is the target.
> - 0002: universal core + pure-QML view, per `vpavlin/logos-skills`.
> - 0003: apps first; no upstream load API needed.
> - 0004: the tool set below, accepted.
> - 0005: the charter rules a single-device tool doesn't need.
>
> The list below is the question as it was asked.

1. **Core style.**
   - Recommend: **universal** module using `modules().dynamic(name)`, built
     with the same builder rev as the host. This replaces the brief's "Qt
     module".
2. **The "start it" step.**
   - Recommend: the tool is `open_app(view)`. It installs the view if needed,
     the view raises `basecamp.apps.launch`, and the core waits for the core to
     be ready.
   - For a core with no view, the model tells the user to open it from the
     Modules view. The alternative is asking upstream for a load API: an
     intent like `basecamp.modules.load`, or exposing `loadModule` to modules.
     **Your call whether to raise that upstream.**
3. **Tool set, revised:**
   - `list_installed()`
   - `list_available(query?)`
   - `install(name)`, which brings dependencies
   - `open_app(view)`
   - `module_methods(module)`, via `getPluginMethods`
   - `call(module, method, args[])`, refused unless the module is ready and the
     method/arity match
   - `status()`, via `modules_state.list_modules`
4. **Consent.**
   - The platform does not prompt for module-driven installs, so the
     plan-then-confirm step in the brief is our only safeguard. Keep it
     mandatory for `install` and for any `call` that isn't read-only.
5. **Target version.**
   - Everything here is v0.3.1. Upstream main is already 20 commits ahead
     (`5fc0720` makes package_downloader start lazily, with a new
     `start()`/`getState()`). Pin to v0.3.1 or track main?

## Spike artifacts

- `docs/spikes/install-and-call/voice_spike/`, `…/voice_spike_ui/`: build with
  `nix build .#lgx-portable` in each.
- `docs/spikes/install-and-call/evidence/`: `run2-autorun.json`, `run2-more.json` (the scripts),
  `run2-log.jsonl` (step-by-step results; the large catalog dump truncated),
  `run{1,2}-basecamp-log-excerpt.txt`.
- To reproduce:
  1. Extract the v0.3.1 AppImage.
  2. Install both spike `.lgx` files by hand into a fresh `--user-dir`.
  3. Drop a script at `/tmp/voice_spike/autorun.json`.
  4. Launch it as shown at the top of this file.
