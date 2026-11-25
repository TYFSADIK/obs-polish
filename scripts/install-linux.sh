#!/usr/bin/env bash
# Installs Polish for the current user: OBS from your distribution or the OBS PPA, and the
# Flatpak OBS if it is installed. Restart OBS afterwards.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
installed=0
for base in "${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins" "$HOME/.var/app/com.obsproject.Studio/config/obs-studio/plugins"; do
  case "$base" in
    *com.obsproject.Studio*) [ -d "$HOME/.var/app/com.obsproject.Studio" ] || continue ;;
  esac
  mkdir -p "$base"
  rm -rf "$base/obs-polish"
  cp -R "$HERE/obs-polish" "$base/"
  echo "Installed to $base/obs-polish"
  installed=1
done
[ "$installed" = 1 ] && echo "Restart OBS, then use Tools > Polish, or add the Polish filter to a camera source."
