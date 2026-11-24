#!/usr/bin/env bash
# Fetches the headers the plugin builds against: obs-studio 30.0.0 (libobs and w32-pthreads
# headers only, sparse) and SIMDe (libobs's maths headers need it on ARM).
# Built against the oldest supported libobs, the same binary loads in OBS 30 to 33.
set -euo pipefail
cd "$(dirname "$0")/.."
OBS_TAG="${OBS_TAG:-30.0.0}"
mkdir -p deps
if [ ! -d deps/obs-studio ]; then
  git clone -q --depth 1 --branch "$OBS_TAG" --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git deps/obs-studio
  git -C deps/obs-studio sparse-checkout set libobs deps/w32-pthreads
fi
if [ ! -d deps/simde ]; then
  git clone -q --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git deps/simde
fi
echo "deps ready: obs-studio $OBS_TAG headers, simde 0.8.2"
