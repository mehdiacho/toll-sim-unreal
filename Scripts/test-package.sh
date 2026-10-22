#!/usr/bin/env bash
# Copy the packaged build to an empty folder and take a still from it, once against the bundled
# replay and once against the live feed. Shipping writes no log next to the exe, so the stills
# are the proof. Usage: Scripts/test-package.sh [archive dir] [out dir]
set -euo pipefail
SRC=${1:-X:/Builds/toll-sim-unreal/Windows}
OUT=${2:-X:/Builds/clean-test}
rm -rf "$OUT"
# robocopy, not cp: Git Bash's cp treats TollSimUE.exe and the TollSimUE folder as one name
MSYS_NO_PATHCONV=1 robocopy "$(cygpath -w "$SRC")" "$(cygpath -w "$OUT")" /E /NFL /NDL /NJH /NJS /NP > /dev/null || [ $? -lt 8 ]
du -sh "$OUT"
EXE="$OUT/TollSimUE.exe"
common=(-RenderOffscreen -ResX=1600 -ResY=900 -TollCam=plaza:A1-G01 -TollCamEye=150,45,3 -TollTime=15 -TollShotDelay=40)
MSYS_NO_PATHCONV=1 "$EXE" "${common[@]}" -TollReplay -TollShot="$OUT/../pkg-replay.png"
MSYS_NO_PATHCONV=1 "$EXE" "${common[@]}" -TollShot="$OUT/../pkg-live.png"
ls -la "$OUT/../pkg-replay.png" "$OUT/../pkg-live.png"
