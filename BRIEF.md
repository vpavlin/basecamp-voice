# basecamp-voice — brief

Handed over from a shrooms session on 2026-10-03. Read this first; nothing has
been built yet. The owner is vaclav@status.im (vpavlin).

## The idea

A very simple Logos Basecamp app: one record button. You say what you want —
"install and run the blockchain module" — it is transcribed, sent to a small
**local** AI model that knows how Basecamp works, and the model drives Basecamp
to do it: finds the package, installs it, loads it, starts the node, and tells
you what it did. You watch it happen live in Basecamp. For people who would
rather say what they want than click around.

The model should be local by default — running on any reasonable hardware —
with the endpoint configurable so a user can point it elsewhere.

## Shape (proposed — challenge it)

```
basecamp-voice (ui_qml view)        record ▸ transcript ▸ plan ▸ live steps ▸ summary
        │ logos.callModule (synchronous — never wait on the network or the model here)
        ▼
basecamp-voice-core (Qt module)     records audio, runs whisper, talks to the model,
        │                           executes the model's tool calls, reports progress
        ├─ whisper.cpp (local)      speech → text
        ├─ OpenAI-compatible HTTP   llama.cpp server or Ollama on this machine by
        │                           default; any URL the user configures
        └─ LogosAPI                 package_manager.*, getMethods, invokeRemoteMethod
```

- **Tools, not prose.** The model gets a handful of tools and the core executes
  them; a small model is reliable with a short, well-described tool set and
  cannot invent actions outside it. Proposed set:
  - `list_installed()`, `list_available(category?)` — package_manager.getPackages
  - `install(packages[])` — resolveDependencies + installPackage(s)Async
  - `module_methods(module)` — the module's `getMethods`
  - `call(module, method, args[])` — invokeRemoteMethod
  - `status()` — what is running
- **Plan first, then act.** Before anything that installs or starts, show the
  plan and wait for a click (or a spoken "yes"). Then stream each step to the
  UI as it happens, then a short summary.
- **Model:** a small instruct model with tool calling, ~3–4B (Qwen-class),
  served by llama.cpp's `llama-server` (OpenAI-compatible, grammar-constrained
  tool calls) or Ollama. Do not bundle a multi-GB model in the .lgx; detect a
  local server, explain how to start one, allow a configured URL.
- **System prompt:** what Basecamp is, modules vs UI plugins, the tool set, and
  worked examples ("install X and start it" → the exact tool sequence).

## What is already known (verified 2026-10-03, against workspace source)

Source: `~/devel/github.com/logos-co/logos-workspace/repos/`. The installed
Basecamp here is the **AppImage v0.3.1** (`~/Downloads/LogosBasecamp-Desktop-v0.3.1-…`);
the workspace source may be newer — confirm each API against what is installed.

- **package_manager** (`logos-package-manager-module/src/package_manager_plugin.h`):
  `getPackages()`, `getPackages(category)`, `getCategories()`,
  `resolveDependencies(names)`, `installPackage(name, dir)`,
  `installPackages(names, dir)`, `installPackageAsync/installPackagesAsync`,
  `installPlugin(path)`.
- **Generic calls and introspection exist.** Basecamp's own backend
  (`logos-basecamp/src/MainUIBackend.cpp`, `getCoreModuleMethods` /
  `callCoreModuleMethod`) does `m_logosAPI->getClient(module)->invokeRemoteMethod(module, method, args…)`
  and `invokeRemoteMethod(module, "getMethods")`. Modules receive the same
  `LogosAPI*` in `initLogos`. Up to 3 positional args in that helper.
- **So the core should be a Qt module** (QObject plugin with `initLogos(LogosAPI*)`,
  like package_manager), not the Qt-free "universal" core style: the universal
  style only gets typed callers for declared dependencies, and this needs to
  call modules it does not know about at build time.
- **Example target:** `logos-blockchain-module` has `generate_user_config(…)`,
  `start(config_path, deployment)`, `stop()`, wallet methods.

## Open questions — answer these first, before building UI

1. **Does calling a just-installed module load it?** Does `getClient(name)` on
   a module that was installed after Basecamp started bring it up, or must
   something load it first (capability_module, `refreshCoreModules`, a
   restart)? This decides whether "install and run" works in one go.
2. Can a module call `package_manager` at all, or is it restricted to the
   Basecamp shell (capability checks)? Try it from a minimal Qt module first.
3. Which install directory does a module pass to `installPackage` so Basecamp
   sees the result (`~/.local/share/Logos/LogosBasecamp/modules` and `/plugins`
   on this machine)?
4. Does the installed v0.3.1 package_manager match the workspace source?

A 30-minute spike — a minimal Qt module with one method that lists packages,
installs one, and calls a method on it — answers 1–4 and decides the design.

## Things learned the hard way today (logos-vpn/basecamp, shrooms_core)

- **The view's calls are synchronous IPC.** Any network or model call there
  freezes Basecamp. Do the work on threads in the core; the view polls short
  "what happened since N" calls several times a second. (shrooms_core's
  `agents::Hub` in `logos-vpn/basecamp/core/src/shrooms_agents.cpp` is a
  working example: background jobs, an event buffer, `jobs()` polling.)
- **The view's sandbox blocks all network access**, and `file://` outside the
  plugin's own directory. Everything external goes through the core.
- **Basecamp ships no Qt Multimedia** — importing a missing QML module makes
  the whole view fail to load. Record in the core with the system's recorder:
  `pw-record --rate 16000 --channels 1 file.wav` (fallbacks parecord,
  arecord); stop with SIGINT so the WAV header is finished. Working code:
  `Hub::recordStart/recordStop` in the file above. Use `posix_spawnp`, not
  fork/exec, inside a multithreaded module.
- **whisper.cpp is installed here**: `~/.local/bin/whisper-cli`, model
  `~/.local/share/whisper/ggml-large-v3-turbo-q5_0.bin` (multilingual,
  Czech fine). Measured: 11 s of speech → ~6 s with `-l <lang>` and
  `-ac <clip-sized window>`; auto language detection doubles it. See
  `logos-vpn/internal/agent/stt.go` for the exact flags. A smaller model
  (`base`/`small`) may be enough for short commands — measure.
- **No local LLM server is installed yet** — llama.cpp or Ollama will be
  needed. 20 cores, 62 GB RAM, Intel Iris Xe, no NVIDIA.
- **Packaging:** `nix build .#lgx-portable` (needs `~/.nix-profile/bin` on
  PATH). Nix flakes see only git-tracked files — `git add` new files before
  building. Install by hand for testing: extract the .lgx's
  `variants/linux-amd64/*` plus `manifest.json` into
  `~/.local/share/Logos/LogosBasecamp/{modules,plugins}/<name>/` and write a
  `variant` file containing `linux-amd64`; then restart Basecamp. Basecamp
  scans those directories; there is no registry.
- **Offscreen view tests:** a harness QML that instantiates `Main` with a
  stand-in `bridge` object (make the bridge a property defaulting to
  `logos`), driven by Qt's `qml` tool with `QT_QPA_PLATFORM=offscreen`; see
  `logos-vpn/basecamp-agents/test/AgentsHarness.qml` and the agents section of
  `logos-vpn/basecamp/test/check.sh`. Catches what qmllint cannot, and can
  save a screenshot.
- Skills worth loading: `logos-basecamp-module`, `inter-module-comm`,
  `basecamp-deploy`, `headless-logoscore` (driving modules without the GUI is
  the fastest way to answer the open questions).

## Working agreements with the owner

- Never `git add -A`; stage explicit paths.
- Tests exercise the production code, not a copy of it; mutation-check the
  ones that matter.
- Anything up for discussion goes in a doc; decisions are asked about, not
  assumed.
- Ask before publishing anything public. Local installs are fine.
