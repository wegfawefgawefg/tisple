#!/usr/bin/env bash
# Download Klaus Dormann's 6502 and 65C02 functional tests (GPL-3.0; used here as test data
# only, never committed: tests/data/ is gitignored) in their ca65 port, set their options and
# assemble them with cc65's ca65/ld65 into tests/data/dormann/:
#   6502_functional_test.bin          every documented NMOS opcode (runs on both cores)
#   65C02_extended_opcodes_test.bin   the 65C02 additions; rkwl_wdc_op = 0 and wdc_op = 0, so
#                                     $x7, $xF, $CB and $DB are tested as the 1-byte NOPs of
#                                     the 65C02s without Rockwell's bit instructions (Apple's)
#   6502_decimal_test_nmos.bin        ADC/SBC in decimal mode, every operand and carry: A and
#                                     the flags as the NMOS chip leaves them
#   6502_decimal_test_65c02.bin       the same as the 65C02 leaves them (N and Z valid)
# Each functional test is a 64 KB image loaded at $0000 and started at $0400; NAME.success
# holds the address of its final `jmp *` (any other `jmp *` or branch to itself is a failure).
# The decimal tests end in `jmp *` with ERROR ($000B) 0 when they pass.
# Needs ca65 and ld65: on PATH or in ~/.local/share/tisple/compilers/cc65/bin.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
out="$root/tests/data/dormann"
commit="966b1a35049f9d8be44ad092ec6d43d5ba1831b3" # amb5l's fork, which has the ca65 port
base="https://raw.githubusercontent.com/amb5l/6502_65C02_functional_tests/$commit/ca65"
cc65_bin="$HOME/.local/share/tisple/compilers/cc65/bin"
if ! command -v ca65 > /dev/null && [ -x "$cc65_bin/ca65" ]; then
    PATH="$cc65_bin:$PATH"
fi
mkdir -p "$out"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

for f in 6502_functional_test 65C02_extended_opcodes_test 6502_decimal_test; do
    curl -fsSL "$base/$f.ca65" -o "$work/$f.ca65"
done
curl -fsSL "$base/example.cfg" -o "$work/example.cfg"

# NAME SOURCE: assemble, link a 64 KB image, write NAME.bin and (functional tests) NAME.success
build() {
    name="$1"
    src="$2"
    sed -i 's/^\(\s*\)success\(\s*;if you get here everything went well\)/tisple_success: success\2/' "$src"
    ca65 -g -o "$work/$name.o" "$src"
    ld65 "$work/$name.o" -o "$out/$name.bin" -C "$work/example.cfg" -Ln "$work/$name.labels"
    if grep -q "tisple_success" "$work/$name.labels"; then
        grep "tisple_success" "$work/$name.labels" | awk '{print $2}' > "$out/$name.success"
    fi
}

build 6502_functional_test "$work/6502_functional_test.ca65"

sed -e 's/^wdc_op = 1/wdc_op = 0/' -e 's/^rkwl_wdc_op = 1/rkwl_wdc_op = 0/' \
    "$work/65C02_extended_opcodes_test.ca65" > "$work/65c02.ca65"
build 65C02_extended_opcodes_test "$work/65c02.ca65"

cp "$work/6502_decimal_test.ca65" "$work/dec_nmos.ca65"
build 6502_decimal_test_nmos "$work/dec_nmos.ca65"
sed -e 's/^cputype = 0 /cputype = 1 /' "$work/6502_decimal_test.ca65" > "$work/dec_c02.ca65"
build 6502_decimal_test_65c02 "$work/dec_c02.ca65"

echo "done:"
ls -l "$out"
for s in "$out"/*.success; do echo "$(basename "$s"): $(cat "$s")"; done
