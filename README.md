# basecamp-voice

Say what you want Basecamp to do: "open eth rpc", "install the token list
module". The command is planned into a few well-defined steps, shown to you,
and run after you confirm. You watch it happen in Basecamp.

Status: all three steps wired and running inside Basecamp v0.3.1.
- **Setup:** after the user confirms the sizes, the core downloads a pinned
  `llama-server`, the language model and the speech model. It picks the
  Vulkan build when the machine has Vulkan.
- **Speech:** Parakeet runs inside the core module.
- **Planning:** the language model plans each request through a JSON schema.

Apps that provide Basecamp intents can be asked directly: "add dentist on
Tuesday at 3" goes to Scala, the calendar, after Basecamp asks you
(`docs/adr/0009-app-intents.md`).

Until setup is done, a fixed-phrase planner handles typed commands. To try it,
see `TESTING.md`. See
`docs/adr/0007` (everything inside Basecamp) and `docs/model-eval.md` (which
model).
Background: `BRIEF.md`, `FINDINGS.md`, decisions in `docs/adr/`.

## Platforms

| Platform | Build | Tested |
|---|---|---|
| Linux x86_64 | CI | yes |
| Linux ARM64 | CI | no |
| macOS Apple Silicon | CI | no; see `docs/macos.md` (the microphone works differently there) |

CI (`.github/workflows/build.yml`) builds every platform and merges them into
one package per module. Tags `v*` become releases.

## Layout

| Path | What |
|---|---|
| `core/` | `basecamp_voice_core`: universal core module. Plans, checks and runs the steps. |
| `view/` | `basecamp_voice`: pure-QML view. Shows the plan, asks before acting, follows the steps, opens apps. |
| `tests/e2e/` | End to end in the real Basecamp v0.3.1, isolated and offscreen |
| `docs/adr/` | Decisions |
| `nix/stt/` | Parakeet (whisper.cpp, pinned) as a static library linked into the core |
| `core/eval/` | Prompt/model evaluation: `eval.py`, `cases.json`, `bench.sh` |
| `docs/spikes/install-and-call/` | The spike that answered the open questions. Its module also drives the e2e test. |

## Build

Both packages target **Basecamp v0.3.1** (docs/adr/0001):

```sh
export PATH=~/.nix-profile/bin:$PATH
(cd core && nix build .#lgx-portable -o result-lgx)
(cd view && nix build .#lgx-portable -o result-lgx)
```

Nix only sees git-tracked files, so `git add` new files before building.

## Test

```sh
(cd core && nix build .#unit-tests)
view/test/check.sh /tmp/basecamp_voice.png
BASECAMP=/path/to/squashfs-root tests/e2e/run.sh
```

- **`nix build .#unit-tests`** runs the production tools, planner and engine
  against a fake Basecamp (`core/tests/fake_basecamp.h`).
- **`view/test/check.sh`** renders the production `Main.qml` offscreen on Qt
  6.9.2 with the design system at v0.3.1's pin, checks what it shows, and saves
  a screenshot. Pass `DS=<logos-design-system>/src/qml` to skip the clone.
- **`tests/e2e/run.sh`** with `SCENARIO=tests/e2e/scenario-model.json`,
  `SCENARIO2=…-model-2.json`, `DONES=2` and
  `SEED="<model.gguf> <parakeet.bin>"` runs the full chain:
  - setup: a real runtime download; the seeded models are accepted after
    their checksums;
  - a model-planned request, confirmed and run;
  - WAV recordings through Parakeet.
- **`tests/e2e/run.sh`** installs both packages into a throwaway `--user-dir`
  of the extracted v0.3.1 AppImage. It drives the core through
  `tests/e2e/scenario.json` while the real view opens apps, then leaves the
  driver log and Basecamp log in the printed directory. It downloads packages
  from the official catalog.

What the e2e run checked on 2026-10-03:
- "open eth rpc" installed eth_rpc_ui with eth_rpc_module (42 MB), opened it
  through the view, and waited until eth_rpc_module was running. About 10 s.
- A call to that module worked.
- These were refused or reported with a clear sentence:
  - an inner `{ok:false}`;
  - a misspelled method;
  - a module that isn't running (refused in 1 ms, without waiting out the
    timeout);
  - an ambiguous app name;
  - a cancelled plan (nothing was changed).

## Commands the stand-in planner understands

- `open <app>`, or `install and open <app>`
- `install <package>`
- `what is installed`
- `status`
- `search <words>`
- `methods of <module>`
- `call <module>.<method>(args)`

Chain commands with `then`.
