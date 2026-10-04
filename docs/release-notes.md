Basecamp Voice: say (or type) what you want Logos Basecamp to do. A model on
your computer plans it, you confirm, and it runs.

**Requires:** Logos Basecamp v0.3.1.

## Packages

Each package covers Linux x86_64, Linux ARM64 and macOS Apple Silicon.

- `basecamp_voice_core.lgx`: the engine (planning, tools, speech-to-text, the model runtime).
- `basecamp_voice.lgx`: the window.

## Installing

See `TESTING.md` in the repository: `scripts/install-local.sh` installs both
packages into Basecamp's data folder. On first open, Basecamp Voice offers to
download the speech model, the language model and the llama.cpp runtime (about
3 GB). Every file is pinned and checked against its sha256.

## Status

- **Linux x86_64:** tested.
- **Linux ARM64:** built, not yet tried on hardware.
- **macOS Apple Silicon:** built, not yet tried on hardware.
