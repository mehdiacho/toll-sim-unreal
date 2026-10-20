#!/usr/bin/env bash
# Back the project up to the NAS: a git bundle of every branch, plus the
# third-party content that is deliberately kept out of git (Content/ThirdParty).
#   Scripts/backup-nas.sh
# Lands in /volume4/v4/toll-sim-unreal/ on host `nassync` (see ~/.ssh/config).
# The NAS has no sftp subsystem, so everything goes through ssh pipes.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
dest=/volume4/v4/toll-sim-unreal
stamp="$(date +%Y%m%d-%H%M%S)"
cd "$here"
ssh nassync "mkdir -p $dest/bundles $dest/thirdparty"
git bundle create - --all | ssh nassync "cat > $dest/bundles/toll-sim-unreal-$stamp.bundle"
if [ -d Content/ThirdParty ]; then
  tar -cf - -C Content ThirdParty | ssh nassync "cat > $dest/thirdparty/ThirdParty-$stamp.tar"
fi
cp assets-manifest.json "$here/Saved/assets-manifest.last-backup.json" 2>/dev/null || true
ssh nassync "cd $dest && ls -t bundles/*.bundle | tail -n +11 | xargs -r rm -f; ls -t thirdparty/*.tar 2>/dev/null | tail -n +4 | xargs -r rm -f; du -sh $dest"
