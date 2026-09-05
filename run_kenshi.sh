#!/usr/bin/env bash
# Launcher for the KenshiMMO client on Linux (Wine).
#
# Usage:
#   WINE=/usr/lib/wine/wine64 KENSHI_EXE=~/.wine/drive_c/Kenshi/Kenshi.exe XAUTHORITY=/run/user/1000/xauth_xxx ./run_kenshi.sh
#
# Tips for headless/desktop Linux with Wine + NVIDIA:
#   * Make sure an X/Wayland display is available (DISPLAY=:0 on a local console).
#   * If running as a non-login session, point XAUTHORITY at the active X cookie
#     (e.g. the one in /run/user/<uid>/), else the app can't open the display.
set -euo pipefail

WINE="${WINE:-/usr/local/bin/wine}"
KENSHI_EXE="${KENSHI_EXE:-$HOME/.wine/drive_c/Kenshi/Kenshi.exe}"
DISPLAY="${DISPLAY:-:0}"

if [ ! -x "$WINE" ]; then
    echo "no executable wine at: $WINE" >&2
    exit 1
fi
if [ ! -f "$KENSHI_EXE" ]; then
    echo "no Kenshi.exe at: $KENSHI_EXE" >&2
    exit 1
fi

cd "$(dirname "$KENSHI_EXE")"
exec "$WINE" "$KENSHI_EXE" "$@"