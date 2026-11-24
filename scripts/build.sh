#!/usr/bin/env bash
# Builds build/obs-polish.plugin (universal: Apple silicon and Intel).
set -euo pipefail
cd "$(dirname "$0")/.."
./scripts/fetch-deps.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
