"""Convert one SingleStepTests 6502 JSON file to the compact binary tisple_tests loads.

usage: python3 -I convert_singlestep.py in.json out.bin

All integers are little-endian. Layout:
  u32 case_count
  per case:
    state initial: u16 pc, u8 s, a, x, y, p, u16 ram_count, ram_count * (u16 addr, u8 value)
    state final:   same
    u8 cycle_count, cycle_count * (u16 addr, u8 value, u8 is_write)
"""

import json
import struct
import sys


def pack_state(state):
    out = struct.pack("<HBBBBBH", state["pc"], state["s"], state["a"], state["x"],
                      state["y"], state["p"], len(state["ram"]))
    for addr, value in state["ram"]:
        out += struct.pack("<HB", addr, value)
    return out


def pack_case(case):
    out = pack_state(case["initial"]) + pack_state(case["final"])
    out += struct.pack("<B", len(case["cycles"]))
    for addr, value, kind in case["cycles"]:
        out += struct.pack("<HBB", addr, value, 1 if kind == "write" else 0)
    return out


def main():
    with open(sys.argv[1]) as f:
        cases = json.load(f)
    with open(sys.argv[2], "wb") as f:
        f.write(struct.pack("<I", len(cases)))
        for case in cases:
            f.write(pack_case(case))


main()
