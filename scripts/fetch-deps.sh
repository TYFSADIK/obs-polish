#!/usr/bin/env bash
# Fetches the headers the plugin builds against: obs-studio (libobs headers only, sparse) and SIMDe.
set -euo pipefail
cd "$(dirname "$0")/.."
OBS_TAG="${OBS_TAG:-33.0.3}"
mkdir -p deps
if [ ! -d deps/obs-studio ]; then
  git clone -q --depth 1 --branch "$OBS_TAG" --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git deps/obs-studio
  git -C deps/obs-studio sparse-checkout set libobs frontend/api
fi
if [ ! -d deps/simde ]; then
  git clone -q --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git deps/simde
fi
echo "deps ready: obs-studio $OBS_TAG, simde 0.8.2"
