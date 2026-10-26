#!/usr/bin/env bash
# Download Zellyn Hunter's a2audit disk (MIT, github.com/zellyn/a2audit) to
# tests/data/a2audit/audit.dsk (gitignored). The tests boot it when Apple's roms are in
# ~/.local/share/tisple/roms/ and check its language card and soft switch results.
# By hand:
#   tisple machine --disk tests/data/a2audit/audit.dsk \
#       --rom ~/.local/share/tisple/roms/Apple2e.rom \
#       --disk-rom ~/.local/share/tisple/roms/DISK2.rom --window
# (without --disk-rom our own boot rom boots it; it needs Apple's main rom either way, since
# it prints through the monitor). Its video tests after the first prompt are visual: space
# flips between two modes and you judge whether they look the same.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/tests/data/a2audit"
commit="main"
mkdir -p "$out"
curl -fsSL "https://raw.githubusercontent.com/zellyn/a2audit/$commit/audit/audit.dsk" \
    -o "$out/audit.dsk.tmp"
size=$(wc -c < "$out/audit.dsk.tmp")
if [ "$size" -ne 143360 ]; then
    echo "audit.dsk is $size bytes, expected 143360" >&2
    rm -f "$out/audit.dsk.tmp"
    exit 1
fi
mv "$out/audit.dsk.tmp" "$out/audit.dsk"
echo "wrote $out/audit.dsk"
