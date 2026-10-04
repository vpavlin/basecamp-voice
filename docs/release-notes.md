Basecamp Voice: say (or type) what you want Logos Basecamp to do. A model on
your computer plans it, you confirm, and it runs.

**Requires:** Logos Basecamp v0.3.1.

## New in 0.3.0

- **Ask apps directly.** Apps that provide Basecamp intents can be spoken to: "add dentist on Tuesday at 3" or
  "what's on my calendar tomorrow" goes to [Scala](https://github.com/vpavlin/scala) (scala_ui 0.10.0 or newer).
  Basecamp asks you to confirm each request.

## Packages

Each package covers Linux x86_64, Linux ARM64 and macOS Apple Silicon.

- `basecamp_voice_core.lgx`: the engine (planning, tools, speech-to-text, the model runtime).
- `basecamp_voice.lgx`: the window.

## Installing

- **macOS:** follow [Trying it on a Mac](https://github.com/vpavlin/basecamp-voice/blob/master/docs/macos.md#trying-it-on-a-mac).
  You need [Basecamp 0.3.1 for Apple Silicon](https://github.com/logos-co/logos-basecamp/releases/download/0.3.1/LogosBasecamp-Desktop-v0.3.1-aeb819-aarch64.dmg).
  Basecamp's Package Manager can install both files with **Install Local Package**.
- **Linux:** see `TESTING.md` in the repository: `scripts/install-local.sh` installs both
packages into Basecamp's data folder. On first open, Basecamp Voice offers to
download the speech model, the language model and the llama.cpp runtime (about
3 GB). Every file is pinned and checked against its sha256.

## Status

- **Linux x86_64:** tested.
- **Linux ARM64:** built, not yet tried on hardware.
- **macOS Apple Silicon:** built, not yet tried on hardware.
