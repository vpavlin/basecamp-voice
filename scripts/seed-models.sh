#!/usr/bin/env bash
# Skip the multi-GB setup download: link already-downloaded model files into
# the core's assets directory. The core still checks size and sha256 before
# using them (it accepts nothing else).
#
#   scripts/seed-models.sh [user-dir] [models-dir]
#
# user-dir:   the Basecamp data directory (default ~/.local/share/Logos/LogosBasecamp)
# models-dir: where the .gguf / parakeet files are
#             (default ~/devel/tmp/basecamp-voice-models, plus ~/.local/share/whisper)
#
# Run it after Basecamp Voice has been opened once (that creates the assets
# directory), then press Download in its setup card: the runtime (~30 MB) is
# fetched, the models are verified in place.
set -euo pipefail
ud=${1:-$HOME/.local/share/Logos/LogosBasecamp}
models=${2:-$HOME/devel/tmp/basecamp-voice-models}
assets=$(ls -d "$ud"/module_data/basecamp_voice_core/*/assets 2>/dev/null | head -1 || true)
[ -n "$assets" ] || { echo "no assets directory under $ud - open Basecamp Voice once first" >&2; exit 1; }
for f in "$models"/Qwen3-4B-Instruct-2507-Q4_K_M.gguf "$models"/Qwen3.5-2B-Q4_K_M.gguf \
         "$HOME"/.local/share/whisper/ggml-parakeet-tdt-0.6b-v3-q4_k.bin; do
    [ -f "$f" ] || { echo "skip (not found): $f"; continue; }
    [ -e "$assets/$(basename "$f")" ] && { echo "already there: $(basename "$f")"; continue; }
    ln "$f" "$assets/" 2>/dev/null || cp "$f" "$assets/"
    echo "seeded $(basename "$f")"
done
