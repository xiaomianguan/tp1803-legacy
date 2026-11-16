#!/bin/bash
# Download the root lists in sources.txt and merge new, unexpired roots into libcore's cacerts.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=${SRC:-$HOME/cm13-build/src}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
i=0
grep -v '^#' "$HERE/sources.txt" | while read -r u; do
  [ -n "$u" ] || continue; i=$((i+1))
  curl -sfL -A "Mozilla/5.0" -o "$TMP/$i" "$u" || echo "warning: failed $u" >&2
done
python3 -I "$HERE/merge-cacerts.py" "$SRC/libcore/luni/src/main/files/cacerts" "$TMP"/*
