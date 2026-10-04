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
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) variant=darwin-arm64; default_dest="$HOME/Library/Application Support/Logos/LogosBasecamp" ;;
    Linux-aarch64) variant=linux-arm64; default_dest="$HOME/.local/share/Logos/LogosBasecamp" ;;
    *) variant=linux-amd64; default_dest="$HOME/.local/share/Logos/LogosBasecamp" ;;
esac
dest=${1:-$default_dest}

# The built packages, or ones downloaded from a release: LGX_DIR=<dir with
# basecamp_voice_core.lgx and basecamp_voice.lgx>.
if [ -n "${LGX_DIR:-}" ]; then core_lgx="$LGX_DIR/basecamp_voice_core.lgx"; view_lgx="$LGX_DIR/basecamp_voice.lgx"
else core_lgx=$(ls "$repo"/core/result-lgx/*.lgx 2>/dev/null | head -1); view_lgx=$(ls "$repo"/view/result-lgx/*.lgx 2>/dev/null | head -1); fi
for f in "$core_lgx" "$view_lgx"; do
    [ -f "$f" ] || { echo "missing $f - build core/ and view/ first" >&2; exit 1; }
done
if pgrep -f "LogosBasecamp" >/dev/null 2>&1 && [ $# -eq 0 ]; then
    echo "Basecamp seems to be running; quit it first (it only scans modules at start)." >&2
    exit 1
fi

install_lgx() {   # <lgx> <dest dir>
    local t; t=$(mktemp -d)
    tar xzf "$1" -C "$t"
    rm -rf "$2"
    mkdir -p "$2"
    [ -d "$t/variants/$variant" ] || { echo "$(basename "$1") has no $variant build" >&2; exit 1; }
    cp -r "$t/variants/$variant"/. "$2"/
    cp "$t"/manifest.json "$2"/
    [ -d "$t"/assets ] && cp -r "$t"/assets "$2"/
    printf '%s' "$variant" > "$2"/variant
    rm -rf "$t"
    echo "installed $(basename "$1") -> $2"
}
install_lgx "$core_lgx" "$dest/modules/basecamp_voice_core"
install_lgx "$view_lgx" "$dest/plugins/basecamp_voice"
echo "Start Basecamp and open 'Basecamp Voice'."
