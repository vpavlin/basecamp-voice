#!/usr/bin/env bash
# End to end in the real Basecamp v0.3.1, isolated and offscreen.
#
# The spike module (docs/spikes/install-and-call/voice_spike) drives
# basecamp_voice_core by name through scenario.json, while the REAL
# basecamp_voice view is open and does its own part (opening apps through the
# shell). Packages come from the official catalog over the network.
#
#   BASECAMP=/path/to/squashfs-root  tests/e2e/run.sh
# BASECAMP is the extracted v0.3.1 AppImage (./AppImage --appimage-extract).
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
BASECAMP=${BASECAMP:?set BASECAMP to the extracted v0.3.1 AppImage (squashfs-root)}
# E2E_DIR: where the throwaway Basecamp data lives (on the same filesystem as
# SEED, so seeding is a hard link). SEED: files to place into the core's
# assets directory once it exists (the core accepts them only after checking
# size and sha256). SCENARIO / SCENARIO2: driver scripts; SCENARIO2 runs once
# seeding is done.
ud=$(mktemp -d "${E2E_DIR:-/tmp}/basecamp-voice-e2e.XXXXXX")
SCENARIO=${SCENARIO:-$repo/tests/e2e/scenario.json}
log=$ud/basecamp.log
drv=/tmp/voice_spike

install_lgx() {   # <lgx> <dest>: what Basecamp's installer leaves on disk
    local t; t=$(mktemp -d)
    mkdir -p "$2"
    tar xzf "$1" -C "$t"
    cp -r "$t"/variants/linux-amd64/. "$2"/
    cp "$t"/manifest.json "$2"/
    [ -d "$t"/assets ] && cp -r "$t"/assets "$2"/
    printf linux-amd64 > "$2"/variant
    rm -rf "$t"
}
install_lgx "$repo"/core/result-lgx/*.lgx "$ud"/modules/basecamp_voice_core
install_lgx "$repo"/view/result-lgx/*.lgx "$ud"/plugins/basecamp_voice
install_lgx "$repo"/docs/spikes/install-and-call/voice_spike/result-lgx/*.lgx "$ud"/modules/voice_spike
install_lgx "$repo"/docs/spikes/install-and-call/voice_spike_ui/result-lgx/*.lgx "$ud"/plugins/voice_spike_ui

rm -rf "$drv"; mkdir -p "$drv"
cp "$SCENARIO" "$drv"/autorun.json

QT_QPA_PLATFORM=offscreen "$BASECAMP"/AppRun --user-dir "$ud" --uri basecamp://app/voice_spike_ui >"$log" 2>&1 &
pid=$!
echo "basecamp pid $pid, user dir $ud"

stop() {   # this instance and its children only
    local all=$pid kids
    kids=$(pgrep -P "$pid" || true)
    while [ -n "$kids" ]; do all="$all $kids"; kids=$(for k in $kids; do pgrep -P "$k" || true; done); done
    kill $all 2>/dev/null || true
    # Anything else started for this data directory (llama-server runs in its
    # own process group).
    sleep 1
    pgrep -f "$ud/" | xargs -r kill 2>/dev/null || true
}
trap stop EXIT

seeded=no
for _ in $(seq 1 1800); do
    if [ "$seeded" = no ] && [ -n "${SCENARIO2:-}" ]; then
        assets=$(ls -d "$ud"/module_data/basecamp_voice_core/*/assets 2>/dev/null | head -1 || true)
        if [ -n "$assets" ]; then
            for f in ${SEED:-}; do ln "$f" "$assets/$(basename "$f")" 2>/dev/null || cp "$f" "$assets/"; done
            cp "$SCENARIO2" "$drv"/seeded.json.tmp && mv "$drv"/seeded.json.tmp "$drv"/seeded.json
            echo "seeded $assets"
            seeded=yes
        fi
    fi
    # The outer script finishes last (nested scripts log "done" first).
    [ "$(grep -c '"script":"done"' "$drv"/log.jsonl 2>/dev/null || true)" -ge "${DONES:-1}" ] 2>/dev/null && break
    sleep 1
done
cp "$drv"/log.jsonl "$ud"/driver.jsonl 2>/dev/null || true
echo "driver log: $ud/driver.jsonl   basecamp log: $log"
