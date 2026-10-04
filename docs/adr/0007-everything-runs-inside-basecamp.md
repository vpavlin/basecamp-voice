# 0007 — Everything runs inside Basecamp

- Status: Accepted (owner, 2026-10-03: "the engine and the actual model run as
  part of Basecamp; I don't want to set it up separately")
- Date: 2026-10-03

## Context

The user installs Basecamp Voice from the catalog and nothing else.

- **The language model needs a runtime.** llama.cpp publishes pinned, checksummed
  prebuilt `llama-server` binaries per platform.
- **Speech-to-text has no prebuilt binary.** Parakeet lives on whisper.cpp
  master, after v1.9.4, and whisper.cpp publishes no Linux binaries.
- **The models are large:** the language model is 2–5 GB, the speech model
  0.4 GB.

## Decision

- **Speech-to-text runs inside the core module.**
  - `nix/stt` builds whisper.cpp at a pinned commit (`60c0be6`). It uses
    portable CPU flags, no OpenMP, and produces one static PIC archive
    (`libvoicestt.a`: parakeet + ggml).
  - The core links it, so there is no extra process or binary.
  - The model loads on first use and stays loaded.
- **The language model runs in the fetched `llama-server`.** As the brief
  decided, it is not bundled:
  - pinned tag `b11379`, one asset per platform, sha256-checked;
  - unpacked into the module's data directory;
  - started on demand on a free localhost port with `--reasoning off`, and
    sleeping after 15 min idle;
  - stopped when the module unloads;
  - a pid file catches a server left over when Basecamp was killed.

  Process isolation means a crash in the model can't take the core down.
- **Downloads only after the user confirms, with the size shown.** The setup
  card lists the runtime and both models.
  - Downloads use libcurl (bundled) against the system CA bundle, and resume
    after an interruption.
  - Each file is checked by size and sha256. A file already present is
    accepted once its checksum matches.
- **Children get a cleaned environment.** Recorders and `llama-server` start
  without Basecamp's AppImage `LD_LIBRARY_PATH`/`LD_PRELOAD`/Qt variables, via
  `posix_spawn`.
- **Recording uses the system's recorder:** `pw-record`, then `parecord`, then
  `arecord`. Basecamp ships no Qt Multimedia, and the view is sandboxed.
- **Remote endpoint.** A user can point Basecamp Voice at any OpenAI-compatible
  endpoint, such as Ollama. The local language model is then not needed; speech
  stays local.

## Rejected

- **Linking llama.cpp into the core.** No GPU variant could be fetched later,
  and a model crash would take the core down.
- **Bundling the models in the `.lgx`.** That means a 3 GB package for users
  who may point at a remote model.
- **Shipping a `parakeet-cli` binary.** A Nix-built executable is not portable
  across distributions, whereas the static library rides on the module's
  portable bundling.

## Consequences

- **The core `.lgx` is about 10 MB:** plugin plus libcurl/OpenSSL and their
  dependencies.
- **Linux x86_64 and arm64, and macOS arm64, have `llama-server` assets
  pinned.** Only Linux x86_64 is built and tested so far.
- **GPU (Vulkan) is a later option:** a second pinned asset, same mechanism.

## Update 2026-10-04: the model runs on the CPU by default

**Decided by the owner.** The Vulkan build is still fetched where the machine
has Vulkan: it also runs on the CPU, so the switch costs nothing later.

**Why:** on the test laptop, with the usual desktop apps open, the Iris Xe read
prompts at 41–89 tokens/s, against about 150 when idle and about 45 on the CPU.
It also wrote more slowly and reset twice ("device lost").

**What the GPU option keeps:**
- **Settings → Run the model on: GPU.**
- Short GPU batches (`--ubatch-size 128`).
- Falling back to the CPU after a GPU failure.
