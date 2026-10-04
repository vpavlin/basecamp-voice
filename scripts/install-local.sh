#!/usr/bin/env bash
# Install the built basecamp_voice_core + basecamp_voice packages into a
# Basecamp data directory, the way Basecamp's installer lays them out.
#
#   scripts/install-local.sh                 # ~/.local/share/Logos/LogosBasecamp (the AppImage's default)
#   scripts/install-local.sh <user-dir>      # a directory you start Basecamp with: --user-dir <user-dir>
#
# Build first: (cd core && nix build .#lgx-portable -o result-lgx); same in view/.
# Quit Basecamp before running this (it reads modules/plugins at start).
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-$HOME/.local/share/Logos/LogosBasecamp}

for f in "$repo"/core/result-lgx/*.lgx "$repo"/view/result-lgx/*.lgx; do
    [ -f "$f" ] || { echo "missing $f - build core/ and view/ first" >&2; exit 1; }
done
if pgrep -f "LogosBasecamp" >/dev/null && [ $# -eq 0 ]; then
    echo "Basecamp seems to be running; quit it first (it only scans modules at start)." >&2
    exit 1
fi

install_lgx() {   # <lgx> <dest dir>
    local t; t=$(mktemp -d)
    tar xzf "$1" -C "$t"
    rm -rf "$2"
    mkdir -p "$2"
    cp -r "$t"/variants/linux-amd64/. "$2"/
    cp "$t"/manifest.json "$2"/
    [ -d "$t"/assets ] && cp -r "$t"/assets "$2"/
    printf linux-amd64 > "$2"/variant
    rm -rf "$t"
    echo "installed $(basename "$1") -> $2"
}
install_lgx "$repo"/core/result-lgx/*.lgx "$dest/modules/basecamp_voice_core"
install_lgx "$repo"/view/result-lgx/*.lgx "$dest/plugins/basecamp_voice"
echo "Start Basecamp and open 'Basecamp Voice'."
