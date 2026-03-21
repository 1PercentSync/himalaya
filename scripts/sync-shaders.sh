#!/usr/bin/env bash
# Sync shaders from source to build directory.
# Usage: ./scripts/sync-shaders.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO_ROOT/shaders"
DST="$REPO_ROOT/cmake-build-debug/app/shaders"

if [ ! -d "$DST" ]; then
    echo "Error: build shader directory does not exist: $DST" >&2
    exit 1
fi

rsync -a --delete "$SRC"/ "$DST"/
echo "Synced: $SRC -> $DST"
