#!/usr/bin/env bash
# Packages a build for one platform into dist/:
#   ./scripts/package.sh macos   <build dir>             obs-polish-<v>-macos-universal.zip
#   ./scripts/package.sh linux   <install prefix> <arch>  obs-polish-<v>-linux-<arch>.tar.gz
#   ./scripts/package.sh windows <install prefix> <arch>  obs-polish-<v>-windows-<arch>.zip
set -euo pipefail
cd "$(dirname "$0")/.."
OS="$1"; SRC="$2"; ARCH="${3:-}"
VERSION=$(sed -n 's/^project(obs-polish VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
STAGE=$(mktemp -d)/obs-polish-$VERSION
mkdir -p "$STAGE" dist
cp README.md LICENSE THIRD_PARTY_NOTICES.md "$STAGE/"
case "$OS" in
  macos)
    cp -R "$SRC/obs-polish.plugin" "$STAGE/"
    rm -f "$STAGE/obs-polish.plugin/Contents/Resources/luts/test-"*
    codesign --force --deep -s - "$STAGE/obs-polish.plugin"
    cp scripts/install.command "$STAGE/Install Polish.command"
    (cd "$(dirname "$STAGE")" && zip -qry "$OLDPWD/dist/obs-polish-$VERSION-macos-universal.zip" "$(basename "$STAGE")")
    ;;
  linux)
    cp -R "$SRC/obs-polish" "$STAGE/"
    cp scripts/install-linux.sh "$STAGE/install.sh"
    chmod +x "$STAGE/install.sh"
    tar -C "$(dirname "$STAGE")" -czf "dist/obs-polish-$VERSION-linux-$ARCH.tar.gz" "$(basename "$STAGE")"
    ;;
  windows)
    cp -R "$SRC/obs-polish" "$STAGE/"
    cp scripts/install-windows.cmd "$STAGE/Install Polish.cmd"
    (cd "$(dirname "$STAGE")" && python -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', '.', sys.argv[2])" "$OLDPWD/dist/obs-polish-$VERSION-windows-$ARCH" "$(basename "$STAGE")")
    ;;
  *) echo "unknown platform $OS"; exit 2 ;;
esac
ls -la dist
