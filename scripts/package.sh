#!/usr/bin/env bash
# Makes dist/obs-polish-<version>-macos.zip: the plugin bundle and a double-click installer.
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(sed -n 's/^project(obs-polish VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
rm -rf build && ./scripts/build.sh
STAGE=$(mktemp -d)/obs-polish
mkdir -p "$STAGE"
cp -R build/obs-polish.plugin "$STAGE/"
codesign --force --deep -s - "$STAGE/obs-polish.plugin"
cp scripts/install.command "$STAGE/"
cp README.md LICENSE "$STAGE/"
mkdir -p dist
(cd "$(dirname "$STAGE")" && zip -qry "$OLDPWD/dist/obs-polish-$VERSION-macos.zip" obs-polish)
echo "dist/obs-polish-$VERSION-macos.zip"
