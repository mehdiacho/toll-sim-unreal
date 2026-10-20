#!/usr/bin/env bash
# Copy the feed data from a toll-sim checkout and regenerate the golden positions.
#   Scripts/sync-data.sh [path/to/toll-sim]   (default ../toll-sim)
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
sim="${1:-$here/../toll-sim}"
cp "$sim/viewer/public/replay.json" "$here/Content/Data/replay.json"
cp "$sim/live/road.json" "$here/Content/Data/road.json"
node "$sim/viewer/scripts/export-golden.mjs" "$here/Content/Data/replay.json" "$here/Content/Data/golden-positions.json"
