# Basecamp Voice on macOS (Apple Silicon)

Status: **built on CI, not yet run on a Mac.** Researched 2026-10-04 against
the Basecamp 0.3.1 DMG and source.

## What is the same as on Linux

- **Basecamp:** `LogosBasecamp-Desktop-v0.3.1-aeb819-aarch64.dmg` from the
  logos-basecamp 0.3.1 release. Signed by Logos Collective Association and
  notarized; macOS 12 or later.
- **Packages:** the same two packages, with a `darwin-arm64` variant inside
  (built by `.github/workflows/build.yml` on `macos-14`).
- **Model runtime:** llama.cpp `b11379`, `llama-b11379-bin-macos-arm64.tar.gz`
  (sha256 pinned).
  - **Metal is built in:** the shaders are embedded in `libggml-metal`.
  - **Self-contained:** the libraries load via `@loader_path`, so it runs
    wherever it is unpacked.
  - **No Gatekeeper prompt:** it is ad-hoc signed only, but runs without one,
    because a file downloaded by the app itself carries no quarantine flag.
  - **Needs macOS 13.3** (its build target), higher than Basecamp's 12.0.
- **Alternatives rejected:**
  - MLX needs a Python runtime and has no standalone server.
  - Ollama is a separate install of several hundred MB.
- **The model runs on the GPU by default on macOS.** Metal on Apple Silicon is
  fast and dependable. On Linux the default is the CPU (ADR 0007).
- **Speech-to-text:** Parakeet built for arm64 on the CPU (NEON). No Metal,
  Accelerate or BLAS, so there is nothing to ship or link.
- **Data folder:** `~/Library/Application Support/Logos/LogosBasecamp`, with
  `modules/`, `plugins/` and `module_data/`.

## The microphone: the one real difference

**Why the core can't record:**
- Basecamp's macOS app declares no microphone use:
  - no `NSMicrophoneUsageDescription` in `app/macos/Info.plist.in`;
  - no `com.apple.security.device.audio-input` in its entitlements.
- macOS holds a process responsible through the app that started it.
  Basecamp's processes, our core included, would be **killed** on opening the
  microphone (`__TCC_CRASHING_DUE_TO_PRIVACY_VIOLATION__`).

**So recording runs in an app of our own:**
- **The helper:** `core/macos/voice_recorder.m`, about 70 lines (AVAudioRecorder,
  16 kHz mono WAV). It is compiled for macOS and embedded in the core's
  library.
- **The bundle:** at first use the core writes
  `module_data/basecamp_voice_core/<id>/Basecamp Voice Recorder.app`, with its
  own `Info.plist` (microphone usage text, `LSUIElement`, so no Dock icon). It
  then ad-hoc signs it with `/usr/bin/codesign`, which seals the `Info.plist`.
- **Launching:** `open -n -g -a <app> --args <wav> <stop-file>`. Started
  through Launch Services, the helper is responsible for itself, and macOS
  asks: *"Basecamp Voice Recorder would like to access the microphone."*
- **Talking to it:** through files beside the recording: `.started`, `.done`,
  `.err` (a sentence for the user, e.g. how to allow the microphone in System
  Settings) and `.stop`.

**What can go wrong (to check on a real Mac):**
- **Writing and signing the bundle,** e.g. `codesign` missing (it is part of
  macOS) or the data folder not writable.
- **The permission prompt not appearing.** In that case the `.err` text says
  where to allow it.
- **Being asked again after an update.** An ad-hoc signed app's permission is
  tied to its exact build, so a new version of the recorder asks again.

**The clean fix belongs upstream**, in logos-basecamp:
- add `NSMicrophoneUsageDescription` to `app/macos/Info.plist.in`;
- add `com.apple.security.device.audio-input` to `LogosBasecamp.entitlements`
  and `logos_host.entitlements`.

Then any module could record in-process, and the helper app would be
unnecessary. Not yet proposed: needs the owner's go-ahead.

## Recipes on macOS

Qt keeps app settings in `~/Library/Preferences/com.logos.<App>.plist` on
macOS, not in `~/.config/Logos/<App>.conf`. Recipe facts are read there with
`/usr/bin/defaults read com.logos.<App> <key>`. The name follows Qt's rule for
an organisation name with no domain; not yet checked on a Mac.

## Trying it on a Mac

1. **Install Basecamp 0.3.1** from the DMG above and open it once.
2. **Download the two packages** from the release:
   `basecamp_voice_core.lgx` and `basecamp_voice.lgx`.
3. **Install the core, then the window,** in either way:
   - **In Basecamp:** Package Manager → **Install Local Package** → pick
     `basecamp_voice_core.lgx` and approve. Then do the same for
     `basecamp_voice.lgx`.
   - **From Terminal,** with Basecamp quit:
     `LGX_DIR=<folder with the two .lgx> scripts/install-local.sh` (from a
     clone of this repository).
4. **Open Basecamp Voice.** Choose a model (Careful 2.5 GB or Fast 1.3 GB) and
   press **Download**.
5. **Type a request** first ("what is installed"), then try **Speak**. macOS
   should ask about the microphone for "Basecamp Voice Recorder".

**What to report back:**
- Did setup finish?
- What do the model figures under a request say ("read … t/s … on the GPU")?
- Did the microphone prompt appear, and was what you said transcribed?
- Any error text, plus `~/Library/Application Support/Logos/LogosBasecamp/module_data/basecamp_voice_core/*/llama-server.log`
  and Basecamp's logs folder `…/LogosBasecamp/logs/`.
