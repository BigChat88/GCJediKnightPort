#!/bin/bash
# Boot the GameCube build in Dolphin with the isolated profile in dolphin-user/.
# Usage: ./run_dolphin.sh [seconds]   (with seconds: run headless-ish, then kill)
ROOT="$(cd "$(dirname "$0")" && pwd)"
DOLPHIN="/d/Retrogames/Dolphin-x64/Dolphin.exe"
rm -rf "$ROOT/dolphin-user/Logs/dolphin.log" "$ROOT/dolphin-user/Dump/Frames"
"$DOLPHIN" -u "$(cygpath -w "$ROOT/dolphin-user")" -b -e "$(cygpath -w "$ROOT/OpenJKDF2/build_gamecube/openjkdf2.dol")" &
PID=$!
if [ -n "$1" ]; then
    sleep "$1"
    taskkill //F //IM Dolphin.exe > /dev/null 2>&1
fi
wait $PID 2>/dev/null
