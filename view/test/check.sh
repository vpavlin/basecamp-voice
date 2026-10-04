#!/usr/bin/env bash
# Render the production view offscreen against test/VoiceHarness.qml and check
# what it shows; saves a screenshot. Uses Qt 6.9.2 (Basecamp v0.3.1's) and the
# design system at Basecamp v0.3.1's pin.
#   QML=/path/to/qml DS=/path/to/logos-design-system/src/qml test/check.sh [shot.png]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
view=$(dirname "$here")
shot=${1:-/tmp/basecamp_voice.png}

QML=${QML:-$(ls /nix/store/*-qtdeclarative-6.9.2/bin/qml 2>/dev/null | head -1)}
[ -x "${QML:-}" ] || { echo "no Qt 6.9.2 qml runtime; set QML=" >&2; exit 1; }
DS=${DS:-}
if [ -z "$DS" ]; then
    DS=$(mktemp -d)/logos-design-system
    git clone -q https://github.com/logos-co/logos-design-system "$DS"
    git -C "$DS" checkout -q 88330d04968954be956311b0be429048a41da2ea
    DS=$DS/src/qml
fi

python3 "$here/qml-plaintext.py" --check "$view/Main.qml"

# A bare nix-store qml binary is not wrapped: point it at its own Qt.
qtbase=$(ldd "$QML" | sed -n 's|.*=> \(/nix/store/[^/]*-qtbase-[^/]*\)/lib/libQt6Core.*|\1|p' | head -1)
qtdecl=$(dirname "$(dirname "$QML")")
if [ -n "$qtbase" ]; then
    export QT_PLUGIN_PATH="$qtbase/lib/qt-6/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
    export QML_IMPORT_PATH="$qtdecl/lib/qt-6/qml${QML_IMPORT_PATH:+:$QML_IMPORT_PATH}"
fi

# Without DISPLAY/WAYLAND_DISPLAY the offscreen platform does not try GLX
# (which aborts in qml's startup); Qt Quick renders in software.
out=$(env -u DISPLAY -u WAYLAND_DISPLAY QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
      QT_ASSUME_STDERR_HAS_CONSOLE=1 timeout 60 "$QML" -I "$DS" "$here/VoiceHarness.qml" -- "$shot" 2>&1) || status=$?
echo "$out" | grep -E "CHECK|RESULT|SHOT|Error|error|is not a type|non-existent|ReferenceError|TypeError" || true
exit ${status:-0}
