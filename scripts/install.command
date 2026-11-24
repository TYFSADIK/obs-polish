#!/usr/bin/env bash
# Double-click to install Polish for the current user. Restart OBS afterwards.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HOME/Library/Application Support/obs-studio/plugins"
mkdir -p "$DEST"
rm -rf "$DEST/obs-polish.plugin"
cp -R "$HERE/obs-polish.plugin" "$DEST/"
xattr -dr com.apple.quarantine "$DEST/obs-polish.plugin" 2>/dev/null || true
echo "Installed to $DEST/obs-polish.plugin"
echo "Restart OBS, then use Tools > Polish, or add the Polish filter to a camera source."
