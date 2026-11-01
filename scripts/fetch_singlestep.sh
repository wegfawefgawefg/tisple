#!/usr/bin/env bash
# Download SingleStepTests/65x02 files and convert them to compact binaries tisple_tests
# loads (gitignored):
#   scripts/fetch_singlestep.sh          the 151 documented NMOS opcodes (6502/v1) into
#                                        tests/data/singlestep/XX.bin
#   scripts/fetch_singlestep.sh 65c02    all 256 opcodes of the 65C02 without the Rockwell bit
#                                        instructions (synertek65c02/v1, the chip Apple's
#                                        enhanced IIe and IIc carry) into tests/data/singlestep65c02
# Each JSON file is about 5 MB; files are fetched, converted, and the JSON deleted again.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
variant="${1:-6502}"
if [ "$variant" = "65c02" ]; then
    out="$root/tests/data/singlestep65c02"
    base="https://raw.githubusercontent.com/SingleStepTests/65x02/main/synertek65c02/v1"
    opcodes="$(for i in $(seq 0 255); do printf '%02x ' "$i"; done)"
else
    out="$root/tests/data/singlestep"
    base="https://raw.githubusercontent.com/SingleStepTests/65x02/main/6502/v1"
    # the 151 documented NMOS opcodes
    opcodes="00 01 05 06 08 09 0a 0d 0e 10 11 15 16 18 19 1d 1e 20 21 24 25 26 28 29 2a 2c 2d 2e
30 31 35 36 38 39 3d 3e 40 41 45 46 48 49 4a 4c 4d 4e 50 51 55 56 58 59 5d 5e 60 61 65 66
68 69 6a 6c 6d 6e 70 71 75 76 78 79 7d 7e 81 84 85 86 88 8a 8c 8d 8e 90 91 94 95 96 98 99
9a 9d a0 a1 a2 a4 a5 a6 a8 a9 aa ac ad ae b0 b1 b4 b5 b6 b8 b9 ba bc bd be c0 c1 c4 c5 c6
c8 c9 ca cc cd ce d0 d1 d5 d6 d8 d9 dd de e0 e1 e4 e5 e6 e8 e9 ea ec ed ee f0 f1 f5 f6 f8
f9 fd fe"
fi
mkdir -p "$out"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fetch_one() {
    op="$1"
    if [ -s "$out/$op.bin" ]; then
        return 0
    fi
    curl -fsSL "$base/$op.json" -o "$work/$op.json"
    python3 -I "$root/scripts/convert_singlestep.py" "$work/$op.json" "$out/$op.bin.tmp"
    mv "$out/$op.bin.tmp" "$out/$op.bin"
    rm -f "$work/$op.json"
}

export -f fetch_one
export out base work root
echo $opcodes | tr ' ' '\n' | xargs -P 4 -I{} bash -c 'fetch_one {}'
echo "done: $(ls "$out"/*.bin | wc -l) files, $(du -sh "$out" | cut -f1) in $out"
